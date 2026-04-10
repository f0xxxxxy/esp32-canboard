#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

/**
 * @brief Initialize WiFi AP mode with configuration timeout.
 * Starts the AP and configuration HTTP server.
 */
void wifi_config_mode_start(void);

/**
 * @brief Notify that a client has connected and reset inactivity timer.
 */
void notify_client_connected(void);

#endif // WIFI_CONFIG_H
