/*
 * repeater_config.h — Runtime configuration stored in NVS
 *
 * Ładuje ustawienia z NVS; jeśli brak → bierze domyślne z menuconfig.
 * Web GUI zapisuje do NVS, po reboot nowe wartości się wczytują.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
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
    /* Band / bandwidth (5 GHz-capable SoCs: ESP32-C5) */
    uint8_t  band_mode;           /* wifi_band_mode_t: 1=2.4G only, 2=5G only, 3=auto */
    uint8_t  bw_2g;               /* wifi_bandwidth_t: 1=WIFI_BW20, 2=WIFI_BW40 */
    uint8_t  bw_5g;               /* wifi_bandwidth_t: 1=WIFI_BW20, 2=WIFI_BW40 */
    uint8_t  rssi_5g_adj;         /* dB margin favouring a 5 GHz AP (0–30) */
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
 * Fill cfg with the compile-time (Kconfig) defaults. No NVS access.
 */
void repeater_config_defaults(repeater_config_t *cfg);

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

/**
 * PHY guard — protects against bricking the board on a bad radio config.
 *
 * A wrong band/protocol/bandwidth combination can hang the CPU hard enough
 * that even reflashing needs a manual BOOT+RESET. Call arm() right before
 * touching the PHY and disarm() once it succeeded. If tripped() returns true
 * at boot, the previous attempt never completed — skip the PHY config and come
 * up on safe defaults (2.4 GHz, 20 MHz) so the GUI stays reachable.
 */
bool repeater_phy_guard_tripped(void);
void repeater_phy_guard_arm(void);
void repeater_phy_guard_disarm(void);

/**
 * Record how far the boot got, in NVS, so it can be read back from the host
 * with `python tools/read_boot_stage.py` when the UART gives nothing.
 *
 *   1 = app_main entered          6 = SoftAP started (APSTA)
 *   2 = NVS + netif + event loop  7 = esp_wifi_connect() issued
 *   3 = wifi driver initialised   8 = HTTP server started
 *   4 = esp_wifi_start() returned 9 = all tasks created, boot complete
 *   5 = PHY config applied
 */
void repeater_boot_stage_set(uint8_t stage);

#ifdef __cplusplus
}
#endif
