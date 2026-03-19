#pragma once
#include <stdint.h>
#include <stdbool.h>

// Two ADS7830 devices expected at 0x48 and 0x49 by default
#define ADS7830_ADDR_0 0x48
#define ADS7830_ADDR_1 0x49

bool ads7830_init(void);
// device_idx: 0 or 1, channel: 0-7, out_raw: 0-255
bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw);
