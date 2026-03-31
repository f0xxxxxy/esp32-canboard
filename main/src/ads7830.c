#include "ads7830.h"
#include "i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ads7830";

static const uint8_t device_addrs[2] = { ADS7830_ADDR_0, ADS7830_ADDR_1 };

bool ads7830_init(void)
{
    // Initialize I2C on SDA=GPIO12, SCL=GPIO13
    if (!i2c_master_init(GPIO_NUM_12, GPIO_NUM_13)) {
        ESP_LOGE(TAG, "I2C init failed");
        return false;
    }
    // Probe devices with a few retries to allow for slow device readiness at boot
    uint8_t tmp;
    for (int i = 0; i < 2; ++i) {
        bool found = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (i2c_master_write_read(device_addrs[i], NULL, 0, &tmp, 1)) {
                ESP_LOGI(TAG, "ADS7830 found at 0x%02x", device_addrs[i]);
                found = true;
                break;
            }
            // small delay between retries
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!found) {
            ESP_LOGW(TAG, "ADS7830 at 0x%02x not responding after retries", device_addrs[i]);
        }
    }
    return true;
}

bool ads7830_read_channel(int device_idx, int channel, uint8_t *out_raw)
{
    if (device_idx < 0 || device_idx > 1 || channel < 0 || channel > 7 || out_raw == NULL) return false;
    uint8_t addr = device_addrs[device_idx];
    // ADS7830 expects a command byte to select channel; use 0x84 | channel for single-ended read (approx)
    uint8_t cmd = 0x84 | (channel & 0x07);
    // Write the command then read one byte
    if (!i2c_master_write_read(addr, &cmd, 1, out_raw, 1)) {
        return false;
    }
    return true;
}
