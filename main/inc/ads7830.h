#pragma once
#include <stdint.h>
#include <stdbool.h>

// Two ADS7830 devices expected on the bus.
// Primary device is fixed at 0x48; secondary is often 0x4A, but some boards use 0x49.
#define ADS7830_ADDR_0 0x48
#define ADS7830_ADDR_1 0x4A
#define ADS7830_ADDR_1_ALT 0x49

bool ads7830_init(void);
// device_idx: 0 or 1, channel: 0-7, out_raw: 0-255
bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw);
// Optional metadata outputs (may be NULL): command byte and resolved device I2C address.
bool ads7830_read_channel_meta(int device_idx, int channel, uint8_t *out_raw, uint8_t *out_cmd, uint8_t *out_addr);
