#include "ads7830.h"
#include "i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ads7830";

static uint8_t device_addrs[2] = { ADS7830_ADDR_0, ADS7830_ADDR_1 };

static bool ads7830_probe_addr(uint8_t addr)
{
    uint8_t tmp = 0;
    return i2c_master_write_read(addr, NULL, 0, &tmp, 1);
}

/* ADS7830 single-ended channel select uses a mux lookup, not linear channel bits. */
static uint8_t ads7830_make_cmd(uint8_t channel)
{
    static const uint8_t mux_lut[8] = { 0, 4, 1, 5, 2, 6, 3, 7 };
    uint8_t mux = mux_lut[channel & 0x07];
    return (uint8_t)(0x84 | (mux << 4));
}

bool ads7830_init(void)
{
    // Initialize I2C on SDA=GPIO12, SCL=GPIO13
    if (!i2c_master_init(GPIO_NUM_12, GPIO_NUM_13)) {
        ESP_LOGE(TAG, "I2C init failed");
        return false;
    }
    // Probe devices with retries to allow for slow device readiness at boot.
    // The second ADS7830 may be strapped as 0x49 or 0x4A depending on board revision.
    for (int i = 0; i < 2; ++i) {
        bool found = false;
        uint8_t addr_to_probe[2] = { device_addrs[i], device_addrs[i] };
        int probe_count = 1;

        if (i == 1) {
            addr_to_probe[0] = ADS7830_ADDR_1;
            addr_to_probe[1] = ADS7830_ADDR_1_ALT;
            probe_count = 2;
        }

        for (int attempt = 0; attempt < 3; ++attempt) {
            for (int p = 0; p < probe_count; ++p) {
                if (ads7830_probe_addr(addr_to_probe[p])) {
                    device_addrs[i] = addr_to_probe[p];
                    ESP_LOGI(TAG, "ADS7830[%d] found at 0x%02x", i, device_addrs[i]);
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
            // small delay between retries
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!found) {
            ESP_LOGW(TAG, "ADS7830[%d] not responding (tried 0x%02x%s)",
                     i,
                     addr_to_probe[0],
                     (probe_count == 2) ? " and 0x49/0x4A alt" : "");
        }
    }
    return true;
}

bool ads7830_read_channel_meta(int device_idx, int channel, uint8_t *out_raw, uint8_t *out_cmd, uint8_t *out_addr)
{
    if (device_idx < 0 || device_idx > 1 || channel < 0 || channel > 7 || out_raw == NULL) return false;
    uint8_t addr = device_addrs[device_idx];
    uint8_t cmd = ads7830_make_cmd((uint8_t)channel);
    uint8_t dummy = 0;

    if (out_cmd) {
        *out_cmd = cmd;
    }
    if (out_addr) {
        *out_addr = addr;
    }

    // First read after channel switch may contain previous conversion; discard it.
    if (!i2c_master_write_read(addr, &cmd, 1, &dummy, 1)) {
        return false;
    }
    if (!i2c_master_write_read(addr, &cmd, 1, out_raw, 1)) {
        return false;
    }
    return true;
}

bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw)
{
    return ads7830_read_channel_meta(device_idx, channel, out_raw, NULL, NULL);
}
