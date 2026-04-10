#include "ads7830.h"
#include "i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ads7830";

static uint8_t device_addrs[2] = { ADS7830_ADDR_0, ADS7830_ADDR_1 };

/**
 * @brief Probe an ADS7830 address by attempting a one-byte read.
 * @param addr 7-bit I2C address to probe.
 * @return true when the device acknowledges; false otherwise.
 */
static bool ads7830_probe_addr(uint8_t addr)
{
    uint8_t tmp = 0;
    return i2c_master_write_read(addr, NULL, 0, &tmp, 1);
}

/**
 * @brief Build ADS7830 command byte for a single-ended channel.
 * @param channel Channel number 0..7.
 * @return Encoded command byte.
 */
static uint8_t ads7830_make_cmd(uint8_t channel)
{
    uint8_t ch = (uint8_t)(channel & 0x07);
    uint8_t encoded = (uint8_t)((ch >> 1) | ((ch & 0x01U) << 2));
    return (uint8_t)(0x84 | (encoded << 4));
}

/**
 * @brief Initialize ADS7830 devices and resolve configured addresses.
 * @return true when initialization completes (devices may still be absent);
 *         false only when I2C initialization fails.
 */
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

/**
 * @brief Read one ADS7830 channel and optionally return metadata.
 * @param device_idx ADS7830 index (0 or 1).
 * @param channel Channel index 0..7.
 * @param out_raw Pointer to converted 8-bit raw sample.
 * @param out_cmd Optional pointer to receive command byte (may be NULL).
 * @param out_addr Optional pointer to receive resolved I2C address (may be NULL).
 * @return true on success; false on validation or I2C error.
 */
bool ads7830_read_channel_meta(int device_idx, int channel, uint8_t *out_raw, uint8_t *out_cmd, uint8_t *out_addr)
{
    if (device_idx < 0 || device_idx > 1 || channel < 0 || channel > 7 || out_raw == NULL) return false;
    uint8_t addr = device_addrs[device_idx];
    uint8_t cmd = ads7830_make_cmd((uint8_t)channel);
    uint8_t s0 = 0;
    uint8_t s1 = 0;
    uint8_t s2 = 0;

    if (out_cmd) {
        *out_cmd = cmd;
    }
    if (out_addr) {
        *out_addr = addr;
    }

    // Select channel.
    if (!i2c_master_write_read(addr, &cmd, 1, NULL, 0)) {
        return false;
    }
    // Allow mux/sample to settle after channel change.
    vTaskDelay(pdMS_TO_TICKS(1));

    // Discard initial samples after channel switch.
    if (!i2c_master_write_read(addr, NULL, 0, &s0, 1)) {
        return false;
    }
    if (!i2c_master_write_read(addr, NULL, 0, &s1, 1)) {
        return false;
    }
    if (!i2c_master_write_read(addr, NULL, 0, &s2, 1)) {
        return false;
    }

    *out_raw = s2;
    return true;
}

/**
 * @brief Read one ADS7830 channel.
 * @param device_idx ADS7830 index (0 or 1).
 * @param channel Channel index 0..7.
 * @param out_raw Pointer to converted 8-bit raw sample.
 * @return true on success; false on validation or I2C error.
 */
bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw)
{
    return ads7830_read_channel_meta(device_idx, channel, out_raw, NULL, NULL);
}
