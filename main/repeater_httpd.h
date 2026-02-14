/*
 * repeater_httpd.h — HTTP configuration server
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the HTTP config server.
 * Call AFTER WiFi is started and STA has (or can get) an IP.
 * Returns ESP_OK or error. No-op if CONFIG_REPEATER_HTTPD_ENABLE is off.
 */
esp_err_t repeater_httpd_start(void);

/**
 * Stop the HTTP config server.
 */
void repeater_httpd_stop(void);

#ifdef __cplusplus
}
#endif
