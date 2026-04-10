#pragma once

#include <stdint.h>
#include <stdbool.h>

/** @brief Primary ADS7830 I2C address. */
#define ADS7830_ADDR_0 0x48
/** @brief Secondary ADS7830 I2C address (common). */
#define ADS7830_ADDR_1 0x4A
/** @brief Secondary ADS7830 alternate I2C address. */
#define ADS7830_ADDR_1_ALT 0x49

/**
 * @brief Initialize ADS7830 devices on the I2C bus.
 * @return true when initialization and probing complete; false on failure.
 */
bool ads7830_init(void);

/**
 * @brief Read one ADS7830 channel.
 * @param device_idx ADS7830 device index (0 or 1).
 * @param channel Channel index on selected device (0..7).
 * @param out_raw Pointer to returned 8-bit ADC code.
 * @return true on successful read; false on error.
 */
bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw);

/**
 * @brief Read one ADS7830 channel with optional metadata outputs.
 * @param device_idx ADS7830 device index (0 or 1).
 * @param channel Channel index on selected device (0..7).
 * @param out_raw Pointer to returned 8-bit ADC code.
 * @param out_cmd Optional pointer to returned ADS7830 command byte (may be NULL).
 * @param out_addr Optional pointer to returned resolved I2C address (may be NULL).
 * @return true on successful read; false on error.
 */
bool ads7830_read_channel_meta(int device_idx, int channel, uint8_t *out_raw, uint8_t *out_cmd, uint8_t *out_addr);
