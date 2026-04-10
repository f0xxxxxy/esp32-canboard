#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_server.h"

#include "inc/inputs.h"

static const char *TAG = "http_api";

/**
 * @brief HTTP GET handler for /api/inputs.
 * @param req HTTP request context.
 * @return ESP_OK on success, or ESP_ERR_NO_MEM when JSON buffer allocation fails.
 */
static esp_err_t inputs_get_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = 4096;
    buf = malloc(buf_len);
    if (!buf) return ESP_ERR_NO_MEM;

    uint16_t readings[NUM_ANALOG_INPUTS];
    if (xSemaphoreTake(filtered_voltages_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(readings, filtered_voltages, sizeof(readings));
        xSemaphoreGive(filtered_voltages_mutex);
    } else {
        memset(readings, 0, sizeof(readings));
    }

    uint16_t v5 = get_v5_rail_mv();

    // Build JSON response payload.
    size_t pos = 0;
    pos += snprintf(buf + pos, buf_len - pos, "{\"v5_rail_mv\":%u,\"channels\":[", v5);
    for (int i = 0; i < NUM_ANALOG_INPUTS; ++i) {
        if (i) pos += snprintf(buf + pos, buf_len - pos, ",");
        if (v5 == 0) {
            pos += snprintf(buf + pos, buf_len - pos, "{\"index\":%d,\"mv\":null}", i);
        } else {
            pos += snprintf(buf + pos, buf_len - pos, "{\"index\":%d,\"mv\":%u}", i, readings[i]);
        }
    }
    pos += snprintf(buf + pos, buf_len - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, strlen(buf));
    free(buf);
    return ESP_OK;
}

/**
 * @brief Register /api/inputs endpoint on an HTTP server instance.
 * @param server HTTP server handle.
 * @return ESP_OK on success; error code from httpd_register_uri_handler otherwise.
 */
esp_err_t register_inputs_uri(httpd_handle_t server)
{
    httpd_uri_t uri = {
        .uri = "/api/inputs",
        .method = HTTP_GET,
        .handler = inputs_get_handler,
        .user_ctx = NULL
    };
    return httpd_register_uri_handler(server, &uri);
}
