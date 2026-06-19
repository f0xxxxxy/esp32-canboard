#include "inc/config.h"
#include <stddef.h> 
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"

#define CONFIG_FILE_PATH "/spiffs/config.bin"
#define TAG "CONFIG"

/**
 * @brief Computes CRC32 checksum of data buffer.
 *
 * Uses polynomial 0xEDB88320 for CRC32 calculation. Processes each byte with
 * bit-by-bit XOR operations. Used to verify integrity of configuration data
 * stored in SPIFFS.
 *
 * @param data Pointer to data buffer
 * @param len Number of bytes to checksum
 *
 * @return CRC32 value (inverted final result)
 *
 * @note This is a software implementation, not hardware-accelerated.
 *       Initial CRC value is 0xFFFFFFFF, final result is inverted.
 *       Complexity: O(8n) where n is buffer length in bytes.
 */
static uint32_t crc32(const void *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc;
}

/**
 * @brief Saves board configuration to SPIFFS with CRC32 integrity check.
 *
 * Writes the board configuration structure to a binary file on SPIFFS filesystem.
 * Calculates and stores CRC32 checksum before writing to detect corruption.
 * File path: /spiffs/config.bin
 *
 * @param cfg Pointer to board_config_t structure to save
 *
 * @return true if configuration was saved successfully
 *         false if cfg is NULL, file write fails, or insufficient data written
 *
 * @note File is flushed and closed immediately after write.
 *       CRC is calculated over all fields except the crc32 field itself.
 *       Logs: INFO (success with CRC), ERROR (failures)
 *
 * @see config_load(), crc32(), board_config_t
 */
bool config_save(const board_config_t *cfg) {
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return false;
    }
    
    board_config_t temp = *cfg;
    temp.crc32 = crc32(&temp, offsetof(board_config_t, crc32));
    
    FILE *f = fopen(CONFIG_FILE_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open config file for writing");
        return false;
    }
    
    size_t written = fwrite(&temp, sizeof(temp), 1, f);
    fclose(f);
    
    if (written != 1) {
        ESP_LOGE(TAG, "Failed to write config data");
        return false;
    }
    
    ESP_LOGI(TAG, "Config saved, CRC=0x%08X", temp.crc32);
    return true;
}

/**
 * @brief Loads board configuration from SPIFFS with CRC32 verification.
 *
 * Reads board configuration binary file from SPIFFS and verifies integrity
 * using CRC32 checksum. Returns false if file missing, unreadable, or corrupted.
 * File path: /spiffs/config.bin
 *
 * @param cfg Pointer to board_config_t structure to populate
 *
 * @return true if configuration was loaded and CRC verified successfully
 *         false if:
 *           - cfg is NULL
 *           - Config file not found
 *           - File read fails (incomplete data)
 *           - CRC32 mismatch (data corruption detected)
 *
 * @note Logs: WARN (file not found), ERROR (read failures or CRC mismatch), INFO (success)
 *       On CRC failure, logs both calculated and stored CRC values for debugging.
 *
 * @see config_save(), config_set_defaults(), crc32(), board_config_t
 */
bool config_load(board_config_t *cfg) {
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return false;
    }
    
    FILE *f = fopen(CONFIG_FILE_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Config file not found, will use defaults");
        return false;
    }
    
    size_t read = fread(cfg, sizeof(*cfg), 1, f);
    fclose(f);
    
    if (read != 1) {
        ESP_LOGE(TAG, "Failed to read config data");
        return false;
    }
    
    uint32_t crc = crc32(cfg, offsetof(board_config_t, crc32));
    if (crc != cfg->crc32) {
        ESP_LOGE(TAG, "CRC mismatch: expected 0x%08X, got 0x%08X", crc, cfg->crc32);
        return false;
    }
    
    ESP_LOGI(TAG, "Config loaded and verified, CRC=0x%08X", cfg->crc32);

    // migrate older versions if necessary
    if (cfg->version < CONFIG_VERSION) {
        ESP_LOGW(TAG, "Migrating config from version %u to %u", cfg->version, CONFIG_VERSION);
        if (cfg->version < 3) {
            // previous versions stored filtering as boolean
            for (int i = 0; i < CONFIG_CHANNELS; ++i) {
                if (cfg->channels[i].filtering) {
                    cfg->channels[i].filtering = FILTER_MED;
                } else {
                    cfg->channels[i].filtering = FILTER_NONE;
                }
            }
        }
        if (cfg->version < 4) {
            for (int i = 0; i < CONFIG_CHANNELS; ++i) {
                cfg->channels[i].emub_tx = EMUB_TX_DISABLED;
            }
        }
        cfg->version = CONFIG_VERSION;
        // update CRC and persist new layout immediately
        cfg->crc32 = crc32(cfg, offsetof(board_config_t, crc32));
        config_save(cfg);
    }

    return true;
}

/**
 * @brief Initializes board configuration with sensible defaults.
 *
 * Sets all configuration fields to default values:
 * - CAN start ID: 0x100
 * - CAN bus speed: 500 kbps
 * - All channels: RAW sensor type, no filtering, no pullup resistor
 * - Channel names: "Input 1", "Input 2", etc.
 *
 * Clears entire structure with memset before populating defaults.
 * Useful for first-time setup or factory reset.
 *
 * @param cfg Pointer to board_config_t structure to initialize
 *
 * @return void
 *         Logs ERROR if cfg is NULL, otherwise logs INFO on success
 *
 * @note Does not save to SPIFFS; use config_save() to persist.
 *       All array fields (channels) are automatically initialized.
 *       CRC32 field is NOT calculated; must save before using.
 *
 * @see config_save(), config_load(), board_config_t
 */
void config_set_defaults(board_config_t *cfg) {
    if (cfg == NULL) {
        ESP_LOGE(TAG, "Invalid config pointer");
        return;
    }
    
    memset(cfg, 0, sizeof(*cfg));
    cfg->version = CONFIG_VERSION;
    cfg->can_start_id = 0x100;
    cfg->can_speed_kbps = 500; // Default CAN speed 500 kbps
    cfg->pullup_vref_mv = 5025; // Default pull-up reference voltage (mV)
    
    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        snprintf(cfg->channels[i].name, CONFIG_NAME_LEN, "Input %d", i + 1);
        cfg->channels[i].pullup_ohms = 0;
        cfg->channels[i].type = SENSOR_RAW;
        cfg->channels[i].filtering = FILTER_NONE; // No filtering by default
        cfg->channels[i].emub_tx = EMUB_TX_DISABLED;
    }
    
    ESP_LOGI(TAG, "Default config set");
}
