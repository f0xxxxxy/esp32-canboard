#pragma once

#include "driver/gpio.h"
#include "driver/twai.h"

#define DRIVECAN_TX_GPIO_NUM       GPIO_NUM_11
#define DRIVECAN_RX_GPIO_NUM       GPIO_NUM_10

/* Default CAN base ID (can be overridden via board config) */
#define CAN_BASEID 0x620

/// TWAI driver handle for CAN bus communication
extern twai_handle_t twai_can;

/// Log tag for CAN module
static const char* can_log = "can";

/// TWAI timing configuration for CAN bus speed
extern twai_timing_config_t t_can_config;
/// TWAI filter configuration for CAN message filtering
extern twai_filter_config_t f_config;
/// TWAI general configuration for CAN bus initialization
extern twai_general_config_t can_config;

/// @brief Helper function to initialize a TWAI message with a given ID
/// @param id CAN message identifier
/// @return Initialized twai_message_t with 8-byte data payload
static inline twai_message_t init_twai_message(uint32_t id) {
    twai_message_t msg = {
        .identifier = id,
        .extd = 0,
        .data_length_code = 8,
        .data = {0}
    };
    return msg;
}

/// @brief Initialize and start TWAI/CAN driver with dynamic speed configuration
/// Reads CAN speed from board_config_t (125/250/500/1000 kbps) and applies appropriate timing.
/// Must be called before creating canTransmit task.
/// @return ESP_OK on success, ESP_FAIL on driver initialization error
esp_err_t can_init(void);

/// @brief FreeRTOS task for transmitting CAN messages
/// Periodically transmits 3 multiplexed CAN messages containing voltage data from all 10 channels.
/// Message format:
/// - Message 1 (base ID): CPU temp (byte 0), firmware revision (byte 1), channels 0-2 (bytes 2-7)
/// - Message 2 (base ID + 1): Channels 3-6 (bytes 0-7)
/// - Message 3 (base ID + 2): Channels 7-9 (bytes 0-5), reserved (bytes 6-7)
/// All voltages encoded as uint16_t little-endian, 0.001V scale.
/// @param arg Unused FreeRTOS task parameter
void canTransmit(void *arg);
