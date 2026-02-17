/*
 * repeater_config.h — Runtime configuration stored in NVS
 *
 * Ładuje ustawienia z NVS; jeśli brak → bierze domyślne z menuconfig.
 * Web GUI zapisuje do NVS, po reboot nowe wartości się wczytują.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REPEATER_SSID_MAX   33   /* 32 chars + NUL */
#define REPEATER_PASS_MAX   65   /* 64 chars + NUL */

typedef struct {
    /* Upstream (STA) */
    char     sta_ssid[REPEATER_SSID_MAX];
    char     sta_pass[REPEATER_PASS_MAX];
    /* Repeater AP */
    char     ap_ssid[REPEATER_SSID_MAX];
    char     ap_pass[REPEATER_PASS_MAX];
    /* Radio */
    uint8_t  tx_power_dbm;        /* 2–20 */
    uint8_t  max_clients;         /* 1–10 */
    /* Security */
    uint8_t  ap_authmode;         /* wifi_auth_mode_t: 2=WPA,3=WPA2,4=WPA/WPA2,7=WPA2/WPA3,6=WPA3 */
    /* AP cloning */
    uint8_t  ap_clone_ssid;       /* 0=off, 1=clone upstream SSID to AP */
    /* Roaming (pseudo-mesh) */
    uint8_t  pseudo_mesh;         /* 0=off, 1=roam to better AP with same SSID */
    int8_t   roam_rssi_threshold; /* dBm, scan when RSSI drops below this */
    uint8_t  roam_hysteresis;     /* dB, new AP must be this much better */
} repeater_config_t;

/**
 * Load config from NVS (or Kconfig defaults if NVS empty).
 * Must be called AFTER nvs_flash_init().
 */
esp_err_t repeater_config_load(repeater_config_t *cfg);

/**
 * Save config to NVS. Returns ESP_OK on success.
 */
esp_err_t repeater_config_save(const repeater_config_t *cfg);

/**
 * Reset NVS config back to Kconfig defaults.
 */
esp_err_t repeater_config_reset(void);

#ifdef __cplusplus
}
#endif
