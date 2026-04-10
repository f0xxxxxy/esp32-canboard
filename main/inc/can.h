#pragma once

#include "driver/gpio.h"
#include "driver/twai.h"

#define DRIVECAN_TX_GPIO_NUM       GPIO_NUM_11
#define DRIVECAN_RX_GPIO_NUM       GPIO_NUM_10

/** @brief Default CAN base ID (can be overridden via board config). */
#define CAN_BASEID 0x620

/** @brief TWAI driver handle for CAN bus communication. */
extern twai_handle_t twai_can;

/** @brief Log tag for CAN module. */
static const char* can_log = "can";

/** @brief TWAI timing configuration for CAN bus speed. */
extern twai_timing_config_t t_can_config;
/** @brief TWAI filter configuration for CAN message filtering. */
extern twai_filter_config_t f_config;
/** @brief TWAI general configuration for CAN bus initialization. */
extern twai_general_config_t can_config;

/**
 * @brief Initialize a TWAI message with a given ID.
 * @param id Standard CAN identifier to assign.
 * @return Initialized TWAI message with 8-byte payload.
 */
static inline twai_message_t init_twai_message(uint32_t id) {
    twai_message_t msg = {
        .identifier = id,
        .extd = 0,
        .data_length_code = 8,
        .data = {0}
    };
    return msg;
}

/**
 * @brief Initialize and start TWAI/CAN driver with dynamic speed configuration.
 * @return ESP_OK on success; ESP_FAIL on initialization failure.
 */
esp_err_t can_init(void);

/**
 * @brief FreeRTOS task for transmitting CAN messages.
 *
 * Sends eight messages starting at base ID:
 * - base+0..base+3: raw analog channel voltages for channels 1..16 (mV, uint16 LE)
 * - base+4..base+7: converted per-channel sensor outputs for channels 1..16
 * @param arg Unused FreeRTOS task argument.
 */
void canTransmit(void *arg);
