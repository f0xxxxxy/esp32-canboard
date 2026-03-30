#include "i2c_master.h"
#include "esp_log.h"
#include "driver/i2c.h"

void i2c_master_scan(void) {
    ESP_LOGI("i2c_scan", "Scanning I2C bus...");
    for (uint8_t addr = 1; addr < 127; ++addr) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI("i2c_scan", "Found device at 0x%02X", addr);
        }
    }
    ESP_LOGI("i2c_scan", "Scan complete.");
}
