
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_twai.h"
#include "inc/config.h"
#include "inc/can.h"
#include "inc/inputs.h"

extern board_config_t board_cfg;

twai_handle_t twai_can;
twai_timing_config_t t_can_config = TWAI_TIMING_CONFIG_500KBITS();
/// Filter configuration: reject all incoming messages (TX-only mode)
twai_filter_config_t f_config = { .acceptance_code = 0xFFFFFFFF, .acceptance_mask = 0x00000000, .single_filter = true };
twai_general_config_t can_config = TWAI_GENERAL_CONFIG_DEFAULT(DRIVECAN_TX_GPIO_NUM, DRIVECAN_RX_GPIO_NUM, TWAI_MODE_NORMAL);

/**
 * @brief Initialize and start TWAI/CAN driver with dynamic speed configuration
 * Reads CAN speed from board_config_t (125/250/500/1000 kbps) and applies appropriate timing.
 * Must be called before creating canTransmit task.
 * @return ESP_OK on success, ESP_FAIL on driver initialization error
 */
esp_err_t can_init(void) {
    // Select timing config based on board configuration
    if (board_cfg.can_speed_kbps == 125) {
        static const twai_timing_config_t temp_config = TWAI_TIMING_CONFIG_125KBITS();
        memcpy(&t_can_config, &temp_config, sizeof(twai_timing_config_t));
        ESP_LOGI(can_log, "CAN speed set to 125 kbps");
    } else if (board_cfg.can_speed_kbps == 250) {
        static const twai_timing_config_t temp_config = TWAI_TIMING_CONFIG_250KBITS();
        memcpy(&t_can_config, &temp_config, sizeof(twai_timing_config_t));
        ESP_LOGI(can_log, "CAN speed set to 250 kbps");
    } else if (board_cfg.can_speed_kbps == 1000) {
        static const twai_timing_config_t temp_config = TWAI_TIMING_CONFIG_1MBITS();
        memcpy(&t_can_config, &temp_config, sizeof(twai_timing_config_t));
        ESP_LOGI(can_log, "CAN speed set to 1000 kbps");
    } else if (board_cfg.can_speed_kbps == 500) {
        // Default to 500 kbps (already configured above)
        ESP_LOGI(can_log, "CAN speed set to 500 kbps");
    } else {
        ESP_LOGW(can_log, "Invalid CAN speed %lu, defaulting to 500 kbps", board_cfg.can_speed_kbps);
        // t_can_config already defaults to 500 kbps
    }

    // Install TWAI driver
    esp_err_t err = twai_driver_install_v2(&can_config, &t_can_config, &f_config, &twai_can);
    if (err != ESP_OK) {
        ESP_LOGE(can_log, "Failed to install TWAI driver: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }

    ESP_LOGI(can_log, "TWAI driver installed");
    return ESP_OK;
}

void canTransmit(void *arg)
{
    ESP_LOGI(can_log, "CAN Transmit Task Started");

    // Prepare TX message templates (base IDs come from board config)
    twai_message_t tx_msg[8];
    for (size_t i = 0; i < 8; ++i) {
        tx_msg[i] = init_twai_message(board_cfg.can_start_id + i);
    }

    while (1) {
        uint16_t voltages_copy[NUM_ANALOG_INPUTS] = {0};

        if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            memcpy(voltages_copy, filtered_voltages, sizeof(voltages_copy));
            xSemaphoreGive(filtered_voltages_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // First 4 messages: pack the 16 analog channel voltages (4 channels per message)
        for (int msg = 0; msg < 4; ++msg) {
            int base = msg * 4;
            for (int j = 0; j < 4; ++j) {
                int idx = base + j;
                uint16_t val = 0;
                if (idx < NUM_ANALOG_INPUTS) val = voltages_copy[idx];
                tx_msg[msg].data[j*2 + 0] = val & 0xFF;
                tx_msg[msg].data[j*2 + 1] = (val >> 8) & 0xFF;
            }
            twai_transmit(&tx_msg[msg], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // Prepare dynamic signals for all 16 channels (0 if not applicable)
        uint16_t dyn[NUM_ANALOG_INPUTS];
        for (int i = 0; i < NUM_ANALOG_INPUTS; ++i) {
            if (board_cfg.channels[i].type == SENSOR_RAW) {
                dyn[i] = 0;
            } else if (board_cfg.channels[i].type == SENSOR_PRESSURE) {
                uint16_t p_kpa_x100 = getSensorPressure(voltages_copy[i],
                                           board_cfg.channels[i].params.pressure.min_mv,
                                           board_cfg.channels[i].params.pressure.max_mv,
                                           board_cfg.channels[i].params.pressure.min_kpa,
                                           board_cfg.channels[i].params.pressure.max_kpa);
                // Convert transmitted value according to selected unit.
                // getSensorPressure() returns kPa * 100 (0.01 kPa resolution).
                uint16_t out_val = p_kpa_x100;
                uint8_t unit = board_cfg.channels[i].params.pressure.pressure_unit;
                if (unit == UNIT_BAR) {
                    // 1 bar = 100 kPa -> bar*100 = (kPa*100) / 100
                    out_val = (uint16_t)((float)p_kpa_x100 / 100.0f + 0.5f);
                } else if (unit == UNIT_PSI) {
                    // 1 psi = 6.89476 kPa -> psi*100 = (kPa*100) / 6.89476
                    out_val = (uint16_t)((float)p_kpa_x100 / 6.89476f + 0.5f);
                }
                dyn[i] = out_val;
            } else if (board_cfg.channels[i].type == SENSOR_NTC) {
                const ntc_table_def_t *t = ntc_get_table(board_cfg.channels[i].params.ntc.table_id);
                int8_t temp = getSensorTemperature(voltages_copy[i], board_cfg.channels[i].pullup_ohms, get_v5_rail_mv(),
                                                   t ? t->points : NULL, t ? t->points_count : 0);
                if (temp == (int8_t)-128) temp = 0;
                dyn[i] = (uint16_t)((int16_t)temp);
            } else {
                dyn[i] = 0;
            }
        }

        // Next 4 messages: pack the 16 dynamic values (4 per message). Leave zeros where not configured.
        for (int msg = 4; msg < 8; ++msg) {
            int base = (msg - 4) * 4;
            for (int j = 0; j < 4; ++j) {
                int idx = base + j;
                uint16_t val = 0;
                if (idx < NUM_ANALOG_INPUTS) val = dyn[idx];
                tx_msg[msg].data[j*2 + 0] = val & 0xFF;
                tx_msg[msg].data[j*2 + 1] = (val >> 8) & 0xFF;
            }
            twai_transmit(&tx_msg[msg], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        vTaskDelay(pdMS_TO_TICKS(80));
    }
}
