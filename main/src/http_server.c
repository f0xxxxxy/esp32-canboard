#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "inc/config.h"
#include "inc/http_server.h"
#include "inc/inputs.h"
#include "inc/wifi_config.h"
#include "esp_wifi.h"
#include "driver/twai.h"
#include "cJSON.h"
#include <sys/param.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * @brief Log tag for HTTP server module
 */
static const char *TAG = "HTTPD";
/**
 * @brief HTTP server handle
 */
static httpd_handle_t server = NULL;
extern board_config_t board_cfg;
extern twai_handle_t twai_can;
extern esp_err_t can_init(void);

static bool apply_runtime_config(const board_config_t *cfg) {
    if (cfg == NULL) {
        return false;
    }

    board_config_t previous_cfg = board_cfg;
    bool can_speed_changed = (previous_cfg.can_speed_kbps != cfg->can_speed_kbps);
    board_cfg = *cfg;

    if (!can_speed_changed) {
        return true;
    }

    esp_err_t err = twai_stop_v2(twai_can);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to stop TWAI before reinit: %s", esp_err_to_name(err));
    }

    err = twai_driver_uninstall_v2(twai_can);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to uninstall TWAI driver: %s", esp_err_to_name(err));
        board_cfg = previous_cfg;
        return false;
    }

    if (can_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize CAN after config update, restoring previous config");
        board_cfg = previous_cfg;
        if (can_init() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore previous CAN configuration");
        }
        return false;
    }

    ESP_LOGI(TAG, "Runtime CAN speed updated to %lu kbps", (unsigned long)board_cfg.can_speed_kbps);
    return true;
}

// Delayed restart task: stops HTTP server and WiFi, then restarts the chip.
static void delayed_restart_task(void *arg) {
    ESP_LOGI(TAG, "Delayed restart task: stopping HTTP server and WiFi");
    stop_http_server();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Restarting now");
    esp_restart();
    vTaskDelete(NULL);
}

/**
 * @brief HTTP GET handler for index.html
 * Serves the web UI from SPIFFS. Supports both standard requests and wildcard routes.
 * @param req HTTP request context
 * @return ESP_OK on success, ESP_FAIL on file not found or allocation error
 */
esp_err_t index_get_handler(httpd_req_t *req) {
    notify_client_connected();
    FILE *f = fopen("/spiffs/index.html", "r");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open index.html");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    // Get file size
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0 || size > 65536) {  // Arbitrary limit to prevent massive allocations
        ESP_LOGE(TAG, "Invalid file size: %ld", size);
        fclose(f);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    
    char *buf = malloc(size);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for file");
        fclose(f);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        ESP_LOGE(TAG, "Failed to read entire file");
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, size);
    free(buf);
    return ESP_OK;
}

/**
 * @brief HTTP GET handler for board configuration (GET /api/config)
 * Returns current configuration as JSON including all channel settings, sensor types, and NTC table references.
 * @param req HTTP request context
 * @return ESP_OK on success, ESP_FAIL on allocation or config load error
 */
esp_err_t config_get_handler(httpd_req_t *req) {
    notify_client_connected();
    board_config_t cfg;
    if (!config_load(&cfg)) {
        config_set_defaults(&cfg);
    }
    
    // Use larger buffer for JSON output
    char *json = malloc(4096);
    if (!json) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    size_t json_pos = 0;
    const size_t json_max = 4096;
    
    // Start JSON object including CAN speed, base ID and pull-up Vref
    json_pos += snprintf(json + json_pos, json_max - json_pos, "{\"can_speed_kbps\":%lu,\"can_start_id\":%lu,\"pullup_vref_mv\":%u,\"channels\":[",
        (unsigned long)cfg.can_speed_kbps, (unsigned long)cfg.can_start_id, (unsigned)cfg.pullup_vref_mv);
    
    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        json_pos += snprintf(json + json_pos, json_max - json_pos,
            "%s{\"name\":\"%s\",\"pullup_ohms\":%lu,\"type\":%u,\"filtering\":%d,\"emub_tx\":%u,\"params\":{",
            i ? "," : "",
            cfg.channels[i].name,
            (unsigned long)cfg.channels[i].pullup_ohms,
            cfg.channels[i].type,
            cfg.channels[i].filtering,
            cfg.channels[i].emub_tx);
            
        if (cfg.channels[i].type == SENSOR_NTC) {
            const ntc_table_def_t* table = ntc_get_table(cfg.channels[i].params.ntc.table_id);
            json_pos += snprintf(json + json_pos, json_max - json_pos, 
                "\"ntc\":{\"table_id\":%u,\"table_name\":\"%s\"}}}",
                cfg.channels[i].params.ntc.table_id,
                table ? table->name : "Unknown");
        } else if (cfg.channels[i].type == SENSOR_PRESSURE) {
            json_pos += snprintf(json + json_pos, json_max - json_pos, 
                "\"pressure\":{\"min_mv\":%u,\"max_mv\":%u,\"min_kpa\":%.2f,\"max_kpa\":%.2f}}}",
                cfg.channels[i].params.pressure.min_mv,
                cfg.channels[i].params.pressure.max_mv,
                cfg.channels[i].params.pressure.min_kpa,
                cfg.channels[i].params.pressure.max_kpa);
        } else {
            json_pos += snprintf(json + json_pos, json_max - json_pos, "\"raw\":{}}}");
        }
        
        // Check for buffer overflow
        if (json_pos >= json_max - 100) {
            ESP_LOGE(TAG, "JSON buffer overflow");
            free(json);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }
    
    snprintf(json + json_pos, json_max - json_pos, "]}\n");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/**
 * @brief HTTP POST handler for board configuration update (POST /api/config)
 * Accepts JSON configuration and validates all channel settings before persisting to SPIFFS.
 * @param req HTTP request context
 * @return ESP_OK on successful save, ESP_FAIL on JSON parse error or config validation failure
 */
esp_err_t config_post_handler(httpd_req_t *req) {
    const size_t REQ_BUF_SIZE = 2048;
    char *buf = malloc(REQ_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate request buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server error");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, REQ_BUF_SIZE - 1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive request body");
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request error");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON received");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    board_config_t cfg;
    config_set_defaults(&cfg);
    
    cJSON *channels = cJSON_GetObjectItem(root, "channels");
    if (!channels || !cJSON_IsArray(channels) || cJSON_GetArraySize(channels) != CONFIG_CHANNELS) {
        ESP_LOGW(TAG, "Invalid channels array in config");
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channels array");
        return ESP_FAIL;
    }
    
    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        cJSON *ch = cJSON_GetArrayItem(channels, i);
        if (!ch) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing channel entry");
            return ESP_FAIL;
        }
        
        cJSON *name = cJSON_GetObjectItem(ch, "name");
        cJSON *pullup = cJSON_GetObjectItem(ch, "pullup_ohms");
        cJSON *type = cJSON_GetObjectItem(ch, "type");
        cJSON *filtering = cJSON_GetObjectItem(ch, "filtering");
        cJSON *emub_tx = cJSON_GetObjectItem(ch, "emub_tx");
        
        if (!cJSON_IsString(name) || !cJSON_IsNumber(pullup) || !cJSON_IsNumber(type) || !cJSON_IsNumber(emub_tx)) {
            ESP_LOGW(TAG, "Invalid channel fields at index %d", i);
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel fields");
            return ESP_FAIL;
        }
        
        strncpy(cfg.channels[i].name, name->valuestring, CONFIG_NAME_LEN - 1);
        cfg.channels[i].name[CONFIG_NAME_LEN - 1] = 0;
        cfg.channels[i].pullup_ohms = (uint32_t)pullup->valueint;
        cfg.channels[i].type = (sensor_type_t)type->valueint;
        if (filtering && cJSON_IsNumber(filtering)) {
            int lvl = filtering->valueint;
            if (lvl < FILTER_NONE) lvl = FILTER_NONE;
            if (lvl > FILTER_HIGH) lvl = FILTER_HIGH;
            cfg.channels[i].filtering = (uint8_t)lvl;
        } else {
            cfg.channels[i].filtering = FILTER_NONE;
        }
        cfg.channels[i].emub_tx = (uint8_t)emub_tx->valueint;
        if (cfg.channels[i].emub_tx > EMUB_TX_CAN_ANALOG_16) {
            cfg.channels[i].emub_tx = EMUB_TX_DISABLED;
        }
        
        cJSON *params = cJSON_GetObjectItem(ch, "params");
        if (!params) {
            ESP_LOGW(TAG, "Missing params for channel %d", i);
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing channel params");
            return ESP_FAIL;
        }
        
        if (cfg.channels[i].type == SENSOR_NTC) {
            cJSON *ntc = cJSON_GetObjectItem(params, "ntc");
            if (!ntc) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing NTC params");
                return ESP_FAIL;
            }
            cJSON *table_id = cJSON_GetObjectItem(ntc, "table_id");
            if (!cJSON_IsNumber(table_id)) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid NTC table_id");
                return ESP_FAIL;
            }
            cfg.channels[i].params.ntc.table_id = (uint8_t)table_id->valueint;
        } else if (cfg.channels[i].type == SENSOR_PRESSURE) {
            cJSON *pressure = cJSON_GetObjectItem(params, "pressure");
            if (!pressure) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pressure params");
                return ESP_FAIL;
            }
            
            cJSON *min_mv = cJSON_GetObjectItem(pressure, "min_mv");
            cJSON *max_mv = cJSON_GetObjectItem(pressure, "max_mv");
            cJSON *min_kpa = cJSON_GetObjectItem(pressure, "min_kpa");
            cJSON *max_kpa = cJSON_GetObjectItem(pressure, "max_kpa");
            
            if (!cJSON_IsNumber(min_mv) || !cJSON_IsNumber(max_mv) || 
                !cJSON_IsNumber(min_kpa) || !cJSON_IsNumber(max_kpa)) {
                cJSON_Delete(root);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid pressure params");
                return ESP_FAIL;
            }
            
            cfg.channels[i].params.pressure.min_mv = (uint16_t)min_mv->valueint;
            cfg.channels[i].params.pressure.max_mv = (uint16_t)max_mv->valueint;
            cfg.channels[i].params.pressure.min_kpa = (float)min_kpa->valuedouble;
            cfg.channels[i].params.pressure.max_kpa = (float)max_kpa->valuedouble;
        }
    }
    
    // Read optional top-level fields: can_speed_kbps and can_start_id
    cJSON *can_speed = cJSON_GetObjectItem(root, "can_speed_kbps");
    if (can_speed && cJSON_IsNumber(can_speed)) {
        cfg.can_speed_kbps = (uint32_t)can_speed->valueint;
    }
    cJSON *can_start = cJSON_GetObjectItem(root, "can_start_id");
    if (can_start) {
        if (cJSON_IsNumber(can_start)) {
            cfg.can_start_id = (uint32_t)can_start->valueint;
        } else if (cJSON_IsString(can_start)) {
            // support hex string like "0x100"
            const char *s = can_start->valuestring;
            if (s && strlen(s) > 0) {
                cfg.can_start_id = (uint32_t)strtoul(s, NULL, 0);
            }
        }
    }

    cJSON *pullup_vref = cJSON_GetObjectItem(root, "pullup_vref_mv");
    if (pullup_vref && cJSON_IsNumber(pullup_vref)) {
        cfg.pullup_vref_mv = (uint16_t)pullup_vref->valueint;
    }

    // Validate unique EMUB TX assignments
    bool emub_seen[EMUB_TX_CAN_ANALOG_16 + 1] = { false };
    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        if (cfg.channels[i].emub_tx > EMUB_TX_DISABLED) {
            if (emub_seen[cfg.channels[i].emub_tx]) {
                ESP_LOGW(TAG, "Duplicate EMUB TX assignment: %u", cfg.channels[i].emub_tx);
                cJSON_Delete(root);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Duplicate EMUB TX assignment");
                return ESP_FAIL;
            }
            emub_seen[cfg.channels[i].emub_tx] = true;
        }
    }

    cJSON_Delete(root);
    free(buf);

    if (!config_save(&cfg)) {
        ESP_LOGE(TAG, "Failed to save configuration");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save config");
        return ESP_FAIL;
    }

    if (!apply_runtime_config(&cfg)) {
        ESP_LOGW(TAG, "Config saved but failed to fully apply at runtime");
    }
    
    ESP_LOGI(TAG, "Configuration updated successfully");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * @brief HTTP POST handler for device reboot (POST /api/reboot)
 * Immediately triggers ESP32 restart via esp_restart().
 * @param req HTTP request context
 * @return ESP_OK (though will not reach if reboot succeeds)
 */
esp_err_t reboot_post_handler(httpd_req_t *req) {
    httpd_resp_sendstr(req, "Rebooting");
    // Schedule a short delayed restart to allow the HTTP response to complete
    // and to shut down services cleanly.
    BaseType_t xr = xTaskCreate(delayed_restart_task, "del_restart", 2048, NULL, 5, NULL);
    if (xr != pdPASS) {
        ESP_LOGW(TAG, "Failed to create restart task; restarting immediately");
        esp_restart();
    }
    return ESP_OK;
}

/**
 * @brief HTTP GET handler for available NTC lookup tables (GET /api/ntc_tables)
 * Returns JSON array of all available NTC thermistor lookup tables with metadata.
 * @param req HTTP request context
 * @return ESP_OK on success, ESP_FAIL on allocation error
 */
esp_err_t ntc_tables_get_handler(httpd_req_t *req) {
    char *json = malloc(2048);
    if (!json) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    size_t json_pos = 0;
    const size_t json_max = 2048;
    
    json_pos += snprintf(json + json_pos, json_max - json_pos, "{\"tables\":[");
    
    for (size_t i = 0; i < NUM_NTC_TABLES; i++) {
        const ntc_table_def_t* table = ntc_get_table(i);
        if (table) {
            json_pos += snprintf(json + json_pos, json_max - json_pos,
                "%s{\"id\":%zu,\"name\":\"%s\",\"description\":\"%s\",\"points\":%zu}",
                i ? "," : "",
                i,
                table->name,
                table->description,
                table->points_count);
            
            if (json_pos >= json_max - 100) {
                ESP_LOGE(TAG, "JSON buffer overflow in NTC tables");
                free(json);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }
    }
    
    snprintf(json + json_pos, json_max - json_pos, "]}\n");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/**
 * @brief HTTP GET handler for live channel values (GET /api/live_values)
 * Returns current measured voltage and computed value (pressure/temperature) per channel.
 */
esp_err_t live_values_get_handler(httpd_req_t *req) {
    notify_client_connected();

    uint16_t voltages_copy[NUM_ADC_CHANNELS] = {0};
    if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(voltages_copy, (const void *)filtered_voltages, sizeof(voltages_copy));
        xSemaphoreGive(filtered_voltages_mutex);
    }

    char *json = malloc(2048);
    if (!json) {
        ESP_LOGE(TAG, "Failed to allocate live values JSON buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t json_pos = 0;
    const size_t json_max = 2048;
    json_pos += snprintf(json + json_pos, json_max - json_pos, "{\"channels\":[");

    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        float voltage_v = (float)voltages_copy[i] / 1000.0f;

        if (board_cfg.channels[i].type == SENSOR_PRESSURE) {
            uint16_t pressure_x100 = getSensorPressure(
                voltages_copy[i],
                board_cfg.channels[i].params.pressure.min_mv,
                board_cfg.channels[i].params.pressure.max_mv,
                board_cfg.channels[i].params.pressure.min_kpa,
                board_cfg.channels[i].params.pressure.max_kpa);
            float pressure_kpa = (float)pressure_x100 / 100.0f;
            json_pos += snprintf(json + json_pos, json_max - json_pos,
                "%s{\"voltage_v\":%.2f,\"value\":\"%.2f kPa\"}",
                i ? "," : "", voltage_v, pressure_kpa);
        } else if (board_cfg.channels[i].type == SENSOR_NTC) {
            const ntc_table_def_t *table = ntc_get_table(board_cfg.channels[i].params.ntc.table_id);
            int8_t temp_c = getSensorTemperature(
                voltages_copy[i],
                board_cfg.channels[i].pullup_ohms,
                board_cfg.pullup_vref_mv,
                table ? table->points : NULL,
                table ? table->points_count : 0);

            int32_t r_ntc = -1;
            if (voltages_copy[i] > 0 && voltages_copy[i] < board_cfg.pullup_vref_mv &&
                board_cfg.channels[i].pullup_ohms > 0 && board_cfg.pullup_vref_mv > 0) {
                float v_ntc = (float)voltages_copy[i] / 1000.0f;
                float v_ref = (float)board_cfg.pullup_vref_mv / 1000.0f;
                float r_ntc_f = ((float)board_cfg.channels[i].pullup_ohms * v_ntc) / (v_ref - v_ntc);
                r_ntc = (int32_t)(r_ntc_f + 0.5f);
            }

            if (temp_c == (int8_t)-128) {
                json_pos += snprintf(json + json_pos, json_max - json_pos,
                    "%s{\"voltage_v\":%.2f,\"value\":\"\"}",
                    i ? "," : "", voltage_v);
            } else {
                if (r_ntc >= 0) {
                    json_pos += snprintf(json + json_pos, json_max - json_pos,
                        "%s{\"voltage_v\":%.2f,\"value\":\"%d \xC2\xB0C (%ld \xCE\xA9)\"}",
                        i ? "," : "", voltage_v, (int)temp_c, (long)r_ntc);
                } else {
                    json_pos += snprintf(json + json_pos, json_max - json_pos,
                        "%s{\"voltage_v\":%.2f,\"value\":\"%d \xC2\xB0C\"}",
                        i ? "," : "", voltage_v, (int)temp_c);
                }
            }
        } else {
            json_pos += snprintf(json + json_pos, json_max - json_pos,
                "%s{\"voltage_v\":%.2f,\"value\":\"\"}",
                i ? "," : "", voltage_v);
        }

        if (json_pos >= json_max - 96) {
            ESP_LOGE(TAG, "JSON buffer overflow in live values");
            free(json);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    snprintf(json + json_pos, json_max - json_pos, "]}\n");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

/**
 * @brief HTTP GET handler to export configuration as downloadable JSON file
 * (GET /api/config/export.json)
 */
esp_err_t config_export_get_handler(httpd_req_t *req) {
    board_config_t cfg;
    if (!config_load(&cfg)) {
        config_set_defaults(&cfg);
    }

    char *json = malloc(4096);
    if (!json) {
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t json_pos = 0;
    const size_t json_max = 4096;
    json_pos += snprintf(json + json_pos, json_max - json_pos, "{\"can_speed_kbps\":%lu,\"can_start_id\":%lu,\"pullup_vref_mv\":%u,\"channels\":[",
        (unsigned long)cfg.can_speed_kbps, (unsigned long)cfg.can_start_id, (unsigned)cfg.pullup_vref_mv);

    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        json_pos += snprintf(json + json_pos, json_max - json_pos,
            "%s{\"name\":\"%s\",\"pullup_ohms\":%lu,\"type\":%u,\"filtering\":%d,\"emub_tx\":%u,\"params\":{",
            i ? "," : "",
            cfg.channels[i].name,
            (unsigned long)cfg.channels[i].pullup_ohms,
            cfg.channels[i].type,
            cfg.channels[i].filtering,
            cfg.channels[i].emub_tx);

        if (cfg.channels[i].type == SENSOR_NTC) {
            json_pos += snprintf(json + json_pos, json_max - json_pos,
                "\"ntc\":{\"table_id\":%u}}}", cfg.channels[i].params.ntc.table_id);
        } else if (cfg.channels[i].type == SENSOR_PRESSURE) {
            json_pos += snprintf(json + json_pos, json_max - json_pos,
                "\"pressure\":{\"min_mv\":%u,\"max_mv\":%u,\"min_kpa\":%.2f,\"max_kpa\":%.2f}}}",
                cfg.channels[i].params.pressure.min_mv,
                cfg.channels[i].params.pressure.max_mv,
                cfg.channels[i].params.pressure.min_kpa,
                cfg.channels[i].params.pressure.max_kpa);
        } else {
            json_pos += snprintf(json + json_pos, json_max - json_pos, "\"raw\":{}}}");
        }

        if (json_pos >= json_max - 100) {
            ESP_LOGE(TAG, "JSON buffer overflow (export)");
            free(json);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
    }

    snprintf(json + json_pos, json_max - json_pos, "]}\n");

    // Set headers to trigger download in browser
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=esp32-canboard-config.json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}


/**
 * @brief HTTP POST handler to import configuration JSON (POST /api/config/import.json)
 * Accepts raw JSON body, validates it, backs up existing config, and saves new config.
 */
esp_err_t config_import_post_handler(httpd_req_t *req) {
    const size_t REQ_BUF_SIZE = 4096;
    char *buf = malloc(REQ_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate import buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server error");
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, REQ_BUF_SIZE - 1);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive import body");
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request error");
        return ESP_FAIL;
    }
    buf[ret] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "Invalid JSON import");
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    board_config_t cfg;
    config_set_defaults(&cfg);

    cJSON *channels = cJSON_GetObjectItem(root, "channels");
    if (!channels || !cJSON_IsArray(channels) || cJSON_GetArraySize(channels) != CONFIG_CHANNELS) {
        ESP_LOGW(TAG, "Invalid channels array in import");
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channels array");
        return ESP_FAIL;
    }

    for (int i = 0; i < CONFIG_CHANNELS; ++i) {
        cJSON *ch = cJSON_GetArrayItem(channels, i);
        if (!ch) {
            cJSON_Delete(root);
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing channel entry");
            return ESP_FAIL;
        }

        cJSON *name = cJSON_GetObjectItem(ch, "name");
        cJSON *pullup = cJSON_GetObjectItem(ch, "pullup_ohms");
        cJSON *type = cJSON_GetObjectItem(ch, "type");
        cJSON *filtering = cJSON_GetObjectItem(ch, "filtering");
        cJSON *emub_tx = cJSON_GetObjectItem(ch, "emub_tx");

        if (!cJSON_IsString(name) || !cJSON_IsNumber(pullup) || !cJSON_IsNumber(type) || !cJSON_IsNumber(emub_tx)) {
            ESP_LOGW(TAG, "Invalid channel fields at index %d", i);
            cJSON_Delete(root);
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid channel fields");
            return ESP_FAIL;
        }

        strncpy(cfg.channels[i].name, name->valuestring, CONFIG_NAME_LEN - 1);
        cfg.channels[i].name[CONFIG_NAME_LEN - 1] = 0;
        cfg.channels[i].pullup_ohms = (uint32_t)pullup->valueint;
        cfg.channels[i].type = (sensor_type_t)type->valueint;
        cfg.channels[i].emub_tx = (uint8_t)emub_tx->valueint;
        if (cfg.channels[i].emub_tx > EMUB_TX_CAN_ANALOG_16) {
            cfg.channels[i].emub_tx = EMUB_TX_DISABLED;
        }
        if (filtering && cJSON_IsNumber(filtering)) {
            int lvl = filtering->valueint;
            if (lvl < FILTER_NONE) lvl = FILTER_NONE;
            if (lvl > FILTER_HIGH) lvl = FILTER_HIGH;
            cfg.channels[i].filtering = (uint8_t)lvl;
        } else {
            cfg.channels[i].filtering = FILTER_NONE;
        }

        cJSON *params = cJSON_GetObjectItem(ch, "params");
        if (!params) {
            ESP_LOGW(TAG, "Missing params for channel %d", i);
            cJSON_Delete(root);
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing channel params");
            return ESP_FAIL;
        }

        if (cfg.channels[i].type == SENSOR_NTC) {
            cJSON *ntc = cJSON_GetObjectItem(params, "ntc");
            if (!ntc) {
                cJSON_Delete(root);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing NTC params");
                return ESP_FAIL;
            }
            cJSON *table_id = cJSON_GetObjectItem(ntc, "table_id");
            if (!cJSON_IsNumber(table_id)) {
                cJSON_Delete(root);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid NTC table_id");
                return ESP_FAIL;
            }
            cfg.channels[i].params.ntc.table_id = (uint8_t)table_id->valueint;
        } else if (cfg.channels[i].type == SENSOR_PRESSURE) {
            cJSON *pressure = cJSON_GetObjectItem(params, "pressure");
            if (!pressure) {
                cJSON_Delete(root);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing pressure params");
                return ESP_FAIL;
            }

            cJSON *min_mv = cJSON_GetObjectItem(pressure, "min_mv");
            cJSON *max_mv = cJSON_GetObjectItem(pressure, "max_mv");
            cJSON *min_kpa = cJSON_GetObjectItem(pressure, "min_kpa");
            cJSON *max_kpa = cJSON_GetObjectItem(pressure, "max_kpa");

            if (!cJSON_IsNumber(min_mv) || !cJSON_IsNumber(max_mv) || 
                !cJSON_IsNumber(min_kpa) || !cJSON_IsNumber(max_kpa)) {
                cJSON_Delete(root);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid pressure params");
                return ESP_FAIL;
            }

            cfg.channels[i].params.pressure.min_mv = (uint16_t)min_mv->valueint;
            cfg.channels[i].params.pressure.max_mv = (uint16_t)max_mv->valueint;
            cfg.channels[i].params.pressure.min_kpa = (float)min_kpa->valuedouble;
            cfg.channels[i].params.pressure.max_kpa = (float)max_kpa->valuedouble;
        }
    }

    cJSON *can_speed = cJSON_GetObjectItem(root, "can_speed_kbps");
    if (can_speed && cJSON_IsNumber(can_speed)) {
        cfg.can_speed_kbps = (uint32_t)can_speed->valueint;
    }
    cJSON *can_start = cJSON_GetObjectItem(root, "can_start_id");
    if (can_start) {
        if (cJSON_IsNumber(can_start)) {
            cfg.can_start_id = (uint32_t)can_start->valueint;
        } else if (cJSON_IsString(can_start)) {
            const char *s = can_start->valuestring;
            if (s && strlen(s) > 0) {
                cfg.can_start_id = (uint32_t)strtoul(s, NULL, 0);
            }
        }
    }

    cJSON *pullup_vref = cJSON_GetObjectItem(root, "pullup_vref_mv");
    if (pullup_vref && cJSON_IsNumber(pullup_vref)) {
        cfg.pullup_vref_mv = (uint16_t)pullup_vref->valueint;
    }

    // Backup existing config file before overwrite
    const char *path = "/spiffs/config.bin";
    const char *bak = "/spiffs/config.bin.bak";
    // Remove any stale backup
    remove(bak);
    if (rename(path, bak) != 0) {
        ESP_LOGW(TAG, "No existing config to backup or rename failed (non-fatal)");
    } else {
        ESP_LOGI(TAG, "Existing config backed up to %s", bak);
    }

    // Try to save new config
    bool saved = config_save(&cfg);
    if (!saved) {
        ESP_LOGE(TAG, "Failed to save imported configuration; attempting restore");
        // Attempt restore from backup
        rename(bak, path);
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save imported config");
        return ESP_FAIL;
    }

    if (!apply_runtime_config(&cfg)) {
        ESP_LOGW(TAG, "Imported config saved but failed to fully apply at runtime");
    }

    cJSON_Delete(root);
    free(buf);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/**
 * @brief Start HTTP server with REST API endpoints and SPIFFS mount
 * Initializes SPIFFS filesystem and starts HTTP server on port 80 (http://192.168.4.1).
 * Registers five URI handlers for configuration management and device control.
 */
void start_http_server(void) {
    // Initialize SPIFFS
    esp_err_t ret = esp_vfs_spiffs_register(&(esp_vfs_spiffs_conf_t){
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true
    });

    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "SPIFFS already mounted");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted successfully");
    }
    
    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    
    ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register URI handlers
    httpd_uri_t index_uri = { 
        .uri = "/", 
        .method = HTTP_GET, 
        .handler = index_get_handler, 
        .user_ctx = NULL 
    };
    httpd_register_uri_handler(server, &index_uri);
    
    httpd_uri_t config_get_uri = { 
        .uri = "/api/config", 
        .method = HTTP_GET, 
        .handler = config_get_handler, 
        .user_ctx = NULL 
    };
    httpd_register_uri_handler(server, &config_get_uri);
    
    httpd_uri_t config_post_uri = { 
        .uri = "/api/config", 
        .method = HTTP_POST, 
        .handler = config_post_handler, 
        .user_ctx = NULL 
    };
    httpd_register_uri_handler(server, &config_post_uri);

    httpd_uri_t config_export_uri = {
        .uri = "/api/config/export.json",
        .method = HTTP_GET,
        .handler = config_export_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_export_uri);

    httpd_uri_t config_import_uri = {
        .uri = "/api/config/import.json",
        .method = HTTP_POST,
        .handler = config_import_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &config_import_uri);
    
    httpd_uri_t reboot_post_uri = { 
        .uri = "/api/reboot", 
        .method = HTTP_POST, 
        .handler = reboot_post_handler, 
        .user_ctx = NULL 
    };
    httpd_register_uri_handler(server, &reboot_post_uri);
    
    httpd_uri_t ntc_tables_uri = { 
        .uri = "/api/ntc_tables", 
        .method = HTTP_GET, 
        .handler = ntc_tables_get_handler, 
        .user_ctx = NULL 
    };
    httpd_register_uri_handler(server, &ntc_tables_uri);

    httpd_uri_t live_values_uri = {
        .uri = "/api/live_values",
        .method = HTTP_GET,
        .handler = live_values_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &live_values_uri);
    
    ESP_LOGI(TAG, "HTTP server started on http://192.168.4.1");
}

/**
 * @brief Stop HTTP server and unmount SPIFFS
 * Gracefully closes HTTP server and unmounts SPIFFS filesystem.
 */
void stop_http_server(void) {
    if (server) {
        ESP_LOGI(TAG, "Stopping HTTP server");
        httpd_stop(server);
        server = NULL;
    }
    
    // Unmount SPIFFS if needed
    esp_vfs_spiffs_unregister(NULL);
    ESP_LOGI(TAG, "SPIFFS unmounted");
}
