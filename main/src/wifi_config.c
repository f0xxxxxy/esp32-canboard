#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "inc/config.h"

#include "inc/http_server.h"
#include "inc/wifi_config.h"

#define WIFI_SSID "ESP32-CanBoard"      // WiFi access point SSID.
#define WIFI_PASS "canboard123"         // WiFi access point password.
#define WIFI_MAX_CONN 1                  // Maximum number of simultaneous WiFi connections.
#define CONFIG_TIMEOUT_MS 120000         // Configuration mode timeout in milliseconds.

/**
 * @brief Log tag for this module
 */
static const char *TAG = "WIFI_CFG";
/**
 * @brief Timer handle for configuration mode timeout
 */
static esp_timer_handle_t config_timer = NULL;
/**
 * @brief Flag indicating whether a client is currently connected to HTTP server
 */
static bool client_connected = false;

/**
 * @brief Timer callback for configuration mode timeout
 * Disables WiFi AP and HTTP server if no client has connected within timeout period.
 * @param arg Callback argument (unused)
 */
static void config_timeout_cb(void *arg) {
    if (!client_connected) {
        ESP_LOGI(TAG, "No client connected in 60s, disabling AP and HTTP server");
        stop_http_server();
        esp_wifi_stop();
    }
}

/**
 * @brief WiFi event handler to log station connect/disconnect and reasons
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base != WIFI_EVENT) return;

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station: %02x:%02x:%02x:%02x:%02x:%02x join, AID=%d, bgn, 40U",
                 evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5], evt->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station: %02x:%02x:%02x:%02x:%02x:%02x leave, AID = %d, reason = %d, bss_flags is %u, bss:0x%p",
                 evt->mac[0], evt->mac[1], evt->mac[2], evt->mac[3], evt->mac[4], evt->mac[5], evt->aid, evt->reason, 0, event_data);
    }
}

/**
 * @brief Initialize WiFi AP mode with configuration timeout
 * Starts WiFi access point (SSID: ESP32-CanBoard) and HTTP configuration server.
 * Automatically shuts down after 60 seconds of inactivity or when shutdown is requested.
 */
void wifi_config_mode_start(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .max_connection = WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP started: %s", WIFI_SSID);

    // Start HTTP server for config UI
    start_http_server();

    // Start 60s timer
    const esp_timer_create_args_t timer_args = {
        .callback = &config_timeout_cb,
        .name = "cfg_timeout"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &config_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(config_timer, CONFIG_TIMEOUT_MS * 1000));
}

/**
 * @brief Notify that a client has connected and reset inactivity timer
 * Call this from HTTP server connection handler to prevent timeout during active configuration.
 */
void notify_client_connected(void) {
    client_connected = true;
    if (config_timer) {
        esp_timer_stop(config_timer);
    }
}
