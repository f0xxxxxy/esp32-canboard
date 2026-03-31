#pragma once
// Minimal I2C master helper (stub)
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

bool i2c_master_init(gpio_num_t sda_gpio, gpio_num_t scl_gpio);
bool i2c_master_write_read(uint8_t addr, const uint8_t *write_data, size_t write_len, uint8_t *read_data, size_t read_len);
