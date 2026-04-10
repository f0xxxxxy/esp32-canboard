#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

/**
 * @brief Initialize the I2C master bus.
 * @param sda_gpio SDA GPIO pin.
 * @param scl_gpio SCL GPIO pin.
 * @return true on success; false on failure.
 */
bool i2c_master_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio);

/**
 * @brief Perform an I2C write-read transaction.
 * @param addr 7-bit I2C device address.
 * @param write_data Pointer to optional TX buffer (may be NULL when write_len is 0).
 * @param write_len Number of bytes to write.
 * @param read_data Pointer to optional RX buffer (may be NULL when read_len is 0).
 * @param read_len Number of bytes to read.
 * @return true on success; false on transaction error.
 */
bool i2c_master_write_read(uint8_t addr, const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len);
