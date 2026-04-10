#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

/**
 * @brief Start HTTP server with REST API endpoints and SPIFFS mount.
 * Serves index.html and exposes /api/config, /api/reboot, and /api/ntc_tables.
 */
void start_http_server(void);

/** @brief Stop HTTP server and unmount SPIFFS. */
void stop_http_server(void);

#endif // HTTP_SERVER_H
