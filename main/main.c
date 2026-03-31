#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/twai.h"

#define USB_PRESENCE_DETECT GPIO_NUM_38

#include "inc/inputs.h"
#include "inc/can.h"

#include "inc/wifi_config.h"
#include "inc/config.h"
#include "esp_spiffs.h"

#include "inc/i2c_master.h"
// Global board configuration (shared with other modules)
board_config_t board_cfg;

void app_main(void)
{
    static const char *log_tag = "APP";
    
    // Load board config from SPIFFS (with CRC check)
    // Ensure SPIFFS is mounted so config_load/config_save can access files
    esp_err_t spiffs_ret = esp_vfs_spiffs_register(&(esp_vfs_spiffs_conf_t){
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    });
    if (spiffs_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(log_tag, "SPIFFS already mounted");
    } else if (spiffs_ret != ESP_OK) {
        ESP_LOGW(log_tag, "Failed to mount SPIFFS: %s", esp_err_to_name(spiffs_ret));
    } else {
        ESP_LOGI(log_tag, "SPIFFS mounted for config access");
    }
    if (!config_load(&board_cfg)) {
        ESP_LOGW(log_tag, "Config CRC invalid or not found, using defaults");
        config_set_defaults(&board_cfg);
        if (!config_save(&board_cfg)) {
            ESP_LOGE(log_tag, "Failed to save default config!");
        }
    } else {
        ESP_LOGI(log_tag, "Config loaded and CRC valid");
    }

    // Start WiFi config mode (AP + HTTP server with timeout)
    wifi_config_mode_start();
    
    // Initialize CPU temperature sensor
    esp_err_t err = initCpuTempSensor();
    if (err != ESP_OK) {
        ESP_LOGW(log_tag, "Failed to initialize CPU temp sensor: %s", esp_err_to_name(err));
    } else {
        int8_t cpu_temp = getCpuTemperature();
        ESP_LOGI(log_tag, "Current CPU Temperature: %d°C", cpu_temp);
    }


    // Initialize ADC channels
    initAdcChannels();

    // I2C scan removed: not required

    // Initialize TWAI/CAN driver with dynamic speed from config
    if (can_init() != ESP_OK) {
        ESP_LOGE(log_tag, "Failed to initialize CAN driver!");
        abort();
    }

    // Create mutex for protecting filtered voltage array
    filtered_voltages_mutex = xSemaphoreCreateMutex();
    if (filtered_voltages_mutex == NULL) {
        ESP_LOGE(log_tag, "Failed to create voltage mutex! Aborting...");
        abort();
    }

    // Process ADCs on Core 1
    BaseType_t task_result = xTaskCreatePinnedToCore(
        adcProcess, "adcProcess", 8192, NULL, 5, NULL, 1);
    if (task_result != pdPASS) {
        ESP_LOGE(log_tag, "Failed to create ADC processing task!");
        abort();
    }

    // Transmit CAN on Core 0
    task_result = xTaskCreatePinnedToCore(
        canTransmit, "canTransmit", 8192, NULL, 10, NULL, 0);
    if (task_result != pdPASS) {
        ESP_LOGE(log_tag, "Failed to create CAN transmit task!");
        abort();
    }
    
    ESP_LOGI(log_tag, "System initialization complete");
}
