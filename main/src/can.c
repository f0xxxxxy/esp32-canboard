
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/twai.h"
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

    // Start TWAI driver
    err = twai_start_v2(twai_can);
    if (err != ESP_OK) {
        ESP_LOGE(can_log, "Failed to start TWAI driver: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(can_log, "TWAI driver started");

    return ESP_OK;
}

/**
 * @brief FreeRTOS task that transmits sensor data over CAN bus.
 *
 * Continuously reads filtered voltage data from all ADC channels and transmits
 * them as a set of CAN messages. New layout:
 * - Message 1 (can_start_id): inputs 0..3 (four uint16 LE)
 * - Message 2 (can_start_id+1): inputs 4..7 (four uint16 LE)
 * - Message 3 (can_start_id+2): inputs 8..9 (two uint16 LE in bytes 0..3)
 *   followed immediately by the first two dynamic signals in bytes 4..7.
 * Dynamic signals (10 total) are encoded based on channel configuration and
 * are placed into msg3 (first two), then msg4 (can_start_id+3, four signals),
 * then msg5 (can_start_id+4, final four signals). Each dynamic signal is 2
 * bytes (uint16/int16 LE) with encoding rules:
 *  - Raw: output 0 (uint16)
 *  - Pressure: unsigned uint16 = pressure_kPa * 100 (factor 0.01)
 *  - NTC: signed int16 = temperature_C * 1 (factor 1)
 *
 * Voltages are encoded as uint16_t little-endian with 0.001 V scale (i.e., mV
 * stored directly). Endianness: little-endian (LSB first) for multi-byte fields.
 *
 * @param arg Unused (FreeRTOS task parameter)
 *
 * @note Runs in infinite loop until task is deleted.
 *       Protected access to filtered_voltages array via mutex.
 *       All CAN transmit failures are logged as warnings (non-fatal).
 *       Message timing: ~1-2ms per message, 20ms inter-cycle delay (total ~25ms = 40Hz capability)
 *       Actual transmission frequency: 20Hz (50ms cycle time)
 */
void canTransmit(void *arg)
{
    ESP_LOGI(can_log, "CAN Transmit Task Started");
<<<<<<< Updated upstream
    extern const uint8_t FIRMWARE_REVISION;
    
    while(1) {
        uint16_t voltages_copy[NUM_ADC_CHANNELS];
        int8_t cpu_temp = 0;
        
        // Safely copy voltage data
=======
    // Setup CAN Packets
    twai_message_t tx_msg[6];
    for (size_t i = 0; i <= 5; ++i) {
        tx_msg[i] = init_twai_message(CAN_BASEID + i);
    }

    while(1) {
        
        uint16_t voltages_copy[NUM_ANALOG_INPUTS];
>>>>>>> Stashed changes
        if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            memcpy(voltages_copy, filtered_voltages, sizeof(voltages_copy));
            xSemaphoreGive(filtered_voltages_mutex);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        
<<<<<<< Updated upstream
        // Get CPU temperature
        cpu_temp = getCpuTemperature();
        
        // Message 1: inputs 0..3 (each uint16 LE)
        twai_message_t msg1 = init_twai_message(board_cfg.can_start_id);
        msg1.data[0] = voltages_copy[0] & 0xFF;  // input 0 LSB
        msg1.data[1] = (voltages_copy[0] >> 8) & 0xFF; // input 0 MSB
        msg1.data[2] = voltages_copy[1] & 0xFF;  // input 1 LSB
        msg1.data[3] = (voltages_copy[1] >> 8) & 0xFF; // input 1 MSB
        msg1.data[4] = voltages_copy[2] & 0xFF;  // input 2 LSB
        msg1.data[5] = (voltages_copy[2] >> 8) & 0xFF; // input 2 MSB
        msg1.data[6] = voltages_copy[3] & 0xFF;  // input 3 LSB
        msg1.data[7] = (voltages_copy[3] >> 8) & 0xFF; // input 3 MSB
        
        esp_err_t err = twai_transmit(&msg1, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(can_log, "Failed to transmit analogVoltage_1: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        
        // Message 2: inputs 4..7 (each uint16 LE)
        twai_message_t msg2 = init_twai_message(board_cfg.can_start_id + 1);
        msg2.data[0] = voltages_copy[4] & 0xFF; // input 4 LSB
        msg2.data[1] = (voltages_copy[4] >> 8) & 0xFF; // input 4 MSB
        msg2.data[2] = voltages_copy[5] & 0xFF; // input 5 LSB
        msg2.data[3] = (voltages_copy[5] >> 8) & 0xFF; // input 5 MSB
        msg2.data[4] = voltages_copy[6] & 0xFF; // input 6 LSB
        msg2.data[5] = (voltages_copy[6] >> 8) & 0xFF; // input 6 MSB
        msg2.data[6] = voltages_copy[7] & 0xFF; // input 7 LSB
        msg2.data[7] = (voltages_copy[7] >> 8) & 0xFF; // input 7 MSB
        
        err = twai_transmit(&msg2, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(can_log, "Failed to transmit analogVoltage_2: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        
        // Message 3: inputs 8..9 (each uint16 LE) and first two dynamic signals
        twai_message_t msg3 = init_twai_message(board_cfg.can_start_id + 2);
        msg3.data[0] = voltages_copy[8] & 0xFF; // input 8 LSB
        msg3.data[1] = (voltages_copy[8] >> 8) & 0xFF; // input 8 MSB
        msg3.data[2] = voltages_copy[9] & 0xFF; // input 9 LSB
        msg3.data[3] = (voltages_copy[9] >> 8) & 0xFF; // input 9 MSB
=======
        // BASE + 3
        tx_msg[3].data[0] = getSensorTemperature(voltages_copy[8], 2400, get_v5_rail_mv(), ntc_table, NTC_TABLE_SIZE(ntc_table)); // Charge Cooler Water Outlet Temperature (C)
        tx_msg[3].data[1] = getSensorTemperature(voltages_copy[9], 2400, get_v5_rail_mv(), ntc_table, NTC_TABLE_SIZE(ntc_table)); // Air Temperature (C)
        tx_msg[3].data[2] = getSensorTemperature(voltages_copy[7], 2400, get_v5_rail_mv(), tmap_table, NTC_TABLE_SIZE(tmap_table)); // Charge Cooler Inlet Temperature (C)
        tx_msg[3].data[7] = getSensorTemperature(voltages_copy[6], 2400, get_v5_rail_mv(), ntc_table, NTC_TABLE_SIZE(ntc_table)); // Charge Cooler Water Inlet Temperature (C)
>>>>>>> Stashed changes

        // Prepare dynamic signals (10 signals, one per channel), encoded per config
        uint16_t dyn[10];
        for (int i = 0; i < 10; ++i) {
            if (board_cfg.channels[i].type == SENSOR_RAW) {
                dyn[i] = 0;
            } else if (board_cfg.channels[i].type == SENSOR_PRESSURE) {
                // getSensorPressure returns kPa * 100 (0.01 kPa resolution)
                uint16_t p = getSensorPressure(voltages_copy[i],
                                               board_cfg.channels[i].params.pressure.min_mv,
                                               board_cfg.channels[i].params.pressure.max_mv,
                                               board_cfg.channels[i].params.pressure.min_kpa,
                                               board_cfg.channels[i].params.pressure.max_kpa);
                dyn[i] = p;
            } else if (board_cfg.channels[i].type == SENSOR_NTC) {
                const ntc_table_def_t *t = ntc_get_table(board_cfg.channels[i].params.ntc.table_id);
                int8_t temp = getSensorTemperature(voltages_copy[i], board_cfg.channels[i].pullup_ohms, board_cfg.pullup_vref_mv,
                                                   t ? t->points : NULL, t ? t->points_count : 0);
                if (temp == (int8_t)-128) temp = 0;
                int16_t t16 = (int16_t)temp;
                dyn[i] = (uint16_t)((uint16_t)t16 & 0xFFFF);
            } else {
                dyn[i] = 0;
            }
        }

        // Place first two dynamic signals into msg3 bytes 4..7
        msg3.data[4] = dyn[0] & 0xFF;
        msg3.data[5] = (dyn[0] >> 8) & 0xFF;
        msg3.data[6] = dyn[1] & 0xFF;
        msg3.data[7] = (dyn[1] >> 8) & 0xFF;

        err = twai_transmit(&msg3, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(can_log, "Failed to transmit analogVoltage_3/msg3: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1));

<<<<<<< Updated upstream
        // Message 4: dynamic signals 2..5 (four uint16)
        twai_message_t msg4 = init_twai_message(board_cfg.can_start_id + 3);
        msg4.data[0] = dyn[2] & 0xFF;
        msg4.data[1] = (dyn[2] >> 8) & 0xFF;
        msg4.data[2] = dyn[3] & 0xFF;
        msg4.data[3] = (dyn[3] >> 8) & 0xFF;
        msg4.data[4] = dyn[4] & 0xFF;
        msg4.data[5] = (dyn[4] >> 8) & 0xFF;
        msg4.data[6] = dyn[5] & 0xFF;
        msg4.data[7] = (dyn[5] >> 8) & 0xFF;

        err = twai_transmit(&msg4, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(can_log, "Failed to transmit dynamic msg4: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(1));

        // Message 5: dynamic signals 6..9 (four uint16)
        twai_message_t msg5 = init_twai_message(board_cfg.can_start_id + 4);
        msg5.data[0] = dyn[6] & 0xFF;
        msg5.data[1] = (dyn[6] >> 8) & 0xFF;
        msg5.data[2] = dyn[7] & 0xFF;
        msg5.data[3] = (dyn[7] >> 8) & 0xFF;
        msg5.data[4] = dyn[8] & 0xFF;
        msg5.data[5] = (dyn[8] >> 8) & 0xFF;
        msg5.data[6] = dyn[9] & 0xFF;
        msg5.data[7] = (dyn[9] >> 8) & 0xFF;

        err = twai_transmit(&msg5, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(can_log, "Failed to transmit dynamic msg5: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
=======
        // BASE + 5 - Remaining analog inputs (10..15) and v5 rail
        uint16_t v5 = get_v5_rail_mv();
        tx_msg[5].data[0] = voltages_copy[10];
        tx_msg[5].data[1] = (voltages_copy[10] >> 8) & 0xFF;
        tx_msg[5].data[2] = voltages_copy[11];
        tx_msg[5].data[3] = (voltages_copy[11] >> 8) & 0xFF;
        tx_msg[5].data[4] = voltages_copy[12];
        tx_msg[5].data[5] = (voltages_copy[12] >> 8) & 0xFF;
        tx_msg[5].data[6] = voltages_copy[13];
        tx_msg[5].data[7] = (voltages_copy[13] >> 8) & 0xFF;
        twai_transmit(&tx_msg[5], pdMS_TO_TICKS(1000));

        // Send an extra message if there are still channels 14..15 to send and v5
        twai_message_t tx_msg_extra = init_twai_message(CAN_BASEID + 6);
        tx_msg_extra.data[0] = voltages_copy[14];
        tx_msg_extra.data[1] = (voltages_copy[14] >> 8) & 0xFF;
        tx_msg_extra.data[2] = voltages_copy[15];
        tx_msg_extra.data[3] = (voltages_copy[15] >> 8) & 0xFF;
        tx_msg_extra.data[4] = v5 & 0xFF;
        tx_msg_extra.data[5] = (v5 >> 8) & 0xFF;
        twai_transmit(&tx_msg_extra, pdMS_TO_TICKS(1000));

        vTaskDelay(pdMS_TO_TICKS(80));
>>>>>>> Stashed changes
    void canTransmit(void *arg)
    {
        ESP_LOGI(can_log, "CAN Transmit Task Started");

        // Prepare TX message templates (base IDs come from board config)
        twai_message_t tx_msg[7];
        for (size_t i = 0; i < 7; ++i) {
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

            // Message 0: inputs 0..3
            tx_msg[0].data[0] = voltages_copy[0] & 0xFF;
            tx_msg[0].data[1] = (voltages_copy[0] >> 8) & 0xFF;
            tx_msg[0].data[2] = voltages_copy[1] & 0xFF;
            tx_msg[0].data[3] = (voltages_copy[1] >> 8) & 0xFF;
            tx_msg[0].data[4] = voltages_copy[2] & 0xFF;
            tx_msg[0].data[5] = (voltages_copy[2] >> 8) & 0xFF;
            tx_msg[0].data[6] = voltages_copy[3] & 0xFF;
            tx_msg[0].data[7] = (voltages_copy[3] >> 8) & 0xFF;
            twai_transmit(&tx_msg[0], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));

            // Message 1: inputs 4..7
            tx_msg[1].data[0] = voltages_copy[4] & 0xFF;
            tx_msg[1].data[1] = (voltages_copy[4] >> 8) & 0xFF;
            tx_msg[1].data[2] = voltages_copy[5] & 0xFF;
            tx_msg[1].data[3] = (voltages_copy[5] >> 8) & 0xFF;
            tx_msg[1].data[4] = voltages_copy[6] & 0xFF;
            tx_msg[1].data[5] = (voltages_copy[6] >> 8) & 0xFF;
            tx_msg[1].data[6] = voltages_copy[7] & 0xFF;
            tx_msg[1].data[7] = (voltages_copy[7] >> 8) & 0xFF;
            twai_transmit(&tx_msg[1], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));

            // Message 2: inputs 8..9 (plus space for dynamic signals later)
            tx_msg[2].data[0] = voltages_copy[8] & 0xFF;
            tx_msg[2].data[1] = (voltages_copy[8] >> 8) & 0xFF;
            tx_msg[2].data[2] = voltages_copy[9] & 0xFF;
            tx_msg[2].data[3] = (voltages_copy[9] >> 8) & 0xFF;

            // Prepare dynamic signals (10 signals for first 10 channels)
            uint16_t dyn[10] = {0};
            for (int i = 0; i < 10; ++i) {
                if (board_cfg.channels[i].type == SENSOR_RAW) {
                    dyn[i] = 0;
                } else if (board_cfg.channels[i].type == SENSOR_PRESSURE) {
                    dyn[i] = getSensorPressure(voltages_copy[i],
                                               board_cfg.channels[i].params.pressure.min_mv,
                                               board_cfg.channels[i].params.pressure.max_mv,
                                               board_cfg.channels[i].params.pressure.min_kpa,
                                               board_cfg.channels[i].params.pressure.max_kpa);
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

            // Place first two dynamic signals into tx_msg[2] bytes 4..7
            tx_msg[2].data[4] = dyn[0] & 0xFF;
            tx_msg[2].data[5] = (dyn[0] >> 8) & 0xFF;
            tx_msg[2].data[6] = dyn[1] & 0xFF;
            tx_msg[2].data[7] = (dyn[1] >> 8) & 0xFF;
            twai_transmit(&tx_msg[2], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));

            // Message 3: dynamic signals 2..5
            tx_msg[3].data[0] = dyn[2] & 0xFF;
            tx_msg[3].data[1] = (dyn[2] >> 8) & 0xFF;
            tx_msg[3].data[2] = dyn[3] & 0xFF;
            tx_msg[3].data[3] = (dyn[3] >> 8) & 0xFF;
            tx_msg[3].data[4] = dyn[4] & 0xFF;
            tx_msg[3].data[5] = (dyn[4] >> 8) & 0xFF;
            tx_msg[3].data[6] = dyn[5] & 0xFF;
            tx_msg[3].data[7] = (dyn[5] >> 8) & 0xFF;
            twai_transmit(&tx_msg[3], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));

            // Message 4: dynamic signals 6..9
            tx_msg[4].data[0] = dyn[6] & 0xFF;
            tx_msg[4].data[1] = (dyn[6] >> 8) & 0xFF;
            tx_msg[4].data[2] = dyn[7] & 0xFF;
            tx_msg[4].data[3] = (dyn[7] >> 8) & 0xFF;
            tx_msg[4].data[4] = dyn[8] & 0xFF;
            tx_msg[4].data[5] = (dyn[8] >> 8) & 0xFF;
            tx_msg[4].data[6] = dyn[9] & 0xFF;
            tx_msg[4].data[7] = (dyn[9] >> 8) & 0xFF;
            twai_transmit(&tx_msg[4], pdMS_TO_TICKS(1000));
            vTaskDelay(pdMS_TO_TICKS(1));

            // Message 5: remaining analog inputs 10..13
            tx_msg[5].data[0] = voltages_copy[10] & 0xFF;
            tx_msg[5].data[1] = (voltages_copy[10] >> 8) & 0xFF;
            tx_msg[5].data[2] = voltages_copy[11] & 0xFF;
            tx_msg[5].data[3] = (voltages_copy[11] >> 8) & 0xFF;
            tx_msg[5].data[4] = voltages_copy[12] & 0xFF;
            tx_msg[5].data[5] = (voltages_copy[12] >> 8) & 0xFF;
            tx_msg[5].data[6] = voltages_copy[13] & 0xFF;
            tx_msg[5].data[7] = (voltages_copy[13] >> 8) & 0xFF;
            twai_transmit(&tx_msg[5], pdMS_TO_TICKS(1000));

            // Message 6: inputs 14..15 and v5 rail
            tx_msg[6].data[0] = voltages_copy[14] & 0xFF;
            tx_msg[6].data[1] = (voltages_copy[14] >> 8) & 0xFF;
            tx_msg[6].data[2] = voltages_copy[15] & 0xFF;
            tx_msg[6].data[3] = (voltages_copy[15] >> 8) & 0xFF;
            uint16_t v5 = get_v5_rail_mv();
            tx_msg[6].data[4] = v5 & 0xFF;
            tx_msg[6].data[5] = (v5 >> 8) & 0xFF;
            twai_transmit(&tx_msg[6], pdMS_TO_TICKS(1000));

            vTaskDelay(pdMS_TO_TICKS(80));
