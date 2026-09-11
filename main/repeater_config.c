/*
 * repeater_config.c — NVS-backed runtime configuration
 *
 * Strategia: najpierw wypełnij strukturę wartościami z Kconfig
 * (repeater_config_defaults), potem nałóż na nią to, co jest w NVS.
 * Dzięki temu dodanie nowego pola nie wymaga duplikowania kodu i stare
 * NVS (bez nowego klucza) automatycznie dostaje sensowny default.
 */
#include <string.h>
#include "repeater_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

static const char *TAG = "rep_cfg";
#define NVS_NAMESPACE "rep_cfg"

/* Kconfig nie definiuje tych symboli na SoC bez 5 GHz — podstaw fallbacki. */
#ifndef CONFIG_REPEATER_BAND_MODE_VAL
#define CONFIG_REPEATER_BAND_MODE_VAL      1   /* WIFI_BAND_MODE_2G_ONLY */
#endif
#ifndef CONFIG_REPEATER_BW_5G_VAL
#define CONFIG_REPEATER_BW_5G_VAL          1   /* WIFI_BW20 */
#endif
#ifndef CONFIG_REPEATER_RSSI_5G_ADJUSTMENT
#define CONFIG_REPEATER_RSSI_5G_ADJUSTMENT 10
#endif

/* ── helpers ─────────────────────────────────────────────────── */

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_sz)
{
    size_t len = dst_sz;
    char tmp[128];
    if (dst_sz > sizeof(tmp)) return;
    if (nvs_get_str(h, key, tmp, &len) == ESP_OK) {
        strlcpy(dst, tmp, dst_sz);
    }
}

static void load_u8(nvs_handle_t h, const char *key, uint8_t *dst)
{
    uint8_t v;
    if (nvs_get_u8(h, key, &v) == ESP_OK) {
        *dst = v;
    }
}

/* ── public API ──────────────────────────────────────────────── */

void repeater_config_defaults(repeater_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strlcpy(cfg->sta_ssid, CONFIG_REPEATER_UPSTREAM_SSID,     sizeof(cfg->sta_ssid));
    strlcpy(cfg->sta_pass, CONFIG_REPEATER_UPSTREAM_PASSWORD, sizeof(cfg->sta_pass));
    strlcpy(cfg->ap_ssid,  CONFIG_REPEATER_AP_SSID,           sizeof(cfg->ap_ssid));
    strlcpy(cfg->ap_pass,  CONFIG_REPEATER_AP_PASSWORD,       sizeof(cfg->ap_pass));

    cfg->tx_power_dbm = CONFIG_REPEATER_TX_POWER;
    cfg->max_clients  = CONFIG_REPEATER_MAX_CLIENTS;
    cfg->ap_authmode  = CONFIG_REPEATER_AP_AUTHMODE_VAL;

    cfg->band_mode    = CONFIG_REPEATER_BAND_MODE_VAL;
    cfg->bw_2g        = CONFIG_REPEATER_BW_2G_VAL;
    cfg->bw_5g        = CONFIG_REPEATER_BW_5G_VAL;
    cfg->rssi_5g_adj  = CONFIG_REPEATER_RSSI_5G_ADJUSTMENT;

#ifdef CONFIG_REPEATER_AP_CLONE_SSID
    cfg->ap_clone_ssid = 1;
#else
    cfg->ap_clone_ssid = 0;
#endif

#ifdef CONFIG_REPEATER_PSEUDO_MESH
    cfg->pseudo_mesh         = 1;
    cfg->roam_rssi_threshold = CONFIG_REPEATER_ROAM_RSSI_THRESHOLD;
    cfg->roam_hysteresis     = CONFIG_REPEATER_ROAM_HYSTERESIS;
#else
    cfg->pseudo_mesh         = 0;
    cfg->roam_rssi_threshold = -70;
    cfg->roam_hysteresis     = 8;
#endif
}

esp_err_t repeater_config_load(repeater_config_t *cfg)
{
    repeater_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No NVS config, using menuconfig defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Loading config from NVS");
    load_str(h, "sta_ssid", cfg->sta_ssid, sizeof(cfg->sta_ssid));
    load_str(h, "sta_pass", cfg->sta_pass, sizeof(cfg->sta_pass));
    load_str(h, "ap_ssid",  cfg->ap_ssid,  sizeof(cfg->ap_ssid));
    load_str(h, "ap_pass",  cfg->ap_pass,  sizeof(cfg->ap_pass));
    load_u8(h, "tx_power",   &cfg->tx_power_dbm);
    load_u8(h, "max_cli",    &cfg->max_clients);
    load_u8(h, "authmode",   &cfg->ap_authmode);
    load_u8(h, "clone_ssid", &cfg->ap_clone_ssid);
    load_u8(h, "pmesh",      &cfg->pseudo_mesh);
    load_u8(h, "roam_hyst",  &cfg->roam_hysteresis);
    load_u8(h, "band_mode",  &cfg->band_mode);
    load_u8(h, "bw_2g",      &cfg->bw_2g);
    load_u8(h, "bw_5g",      &cfg->bw_5g);
    load_u8(h, "adj5g",      &cfg->rssi_5g_adj);
    {
        uint8_t thr = (uint8_t)cfg->roam_rssi_threshold;
        load_u8(h, "roam_rssi", &thr);
        cfg->roam_rssi_threshold = (int8_t)thr;
    }

    nvs_close(h);

    /* ── Sanity clamps: NVS z innej wersji firmware'u nie może
     *    wstawić wartości poza zakresem obsługiwanym przez sprzęt. ── */
    if (cfg->tx_power_dbm < 2 || cfg->tx_power_dbm > 20) cfg->tx_power_dbm = 20;
    if (cfg->max_clients < 1 || cfg->max_clients > 10)   cfg->max_clients = 4;
    if (cfg->bw_2g != 1 && cfg->bw_2g != 2)              cfg->bw_2g = CONFIG_REPEATER_BW_2G_VAL;
    if (cfg->bw_5g != 1 && cfg->bw_5g != 2)              cfg->bw_5g = CONFIG_REPEATER_BW_5G_VAL;
    if (cfg->rssi_5g_adj > 30)                           cfg->rssi_5g_adj = 10;
#if SOC_WIFI_SUPPORT_5G
    if (cfg->band_mode < 1 || cfg->band_mode > 3)        cfg->band_mode = 3;
#else
    cfg->band_mode = 1;   /* SoC bez 5 GHz: zawsze 2.4 GHz only */
    cfg->bw_5g     = 1;
#endif

    return ESP_OK;
}

esp_err_t repeater_config_save(const repeater_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "sta_ssid", cfg->sta_ssid);
    nvs_set_str(h, "sta_pass", cfg->sta_pass);
    nvs_set_str(h, "ap_ssid",  cfg->ap_ssid);
    nvs_set_str(h, "ap_pass",  cfg->ap_pass);
    nvs_set_u8(h,  "tx_power", cfg->tx_power_dbm);
    nvs_set_u8(h,  "max_cli",  cfg->max_clients);
    nvs_set_u8(h,  "authmode", cfg->ap_authmode);
    nvs_set_u8(h,  "clone_ssid", cfg->ap_clone_ssid);
    nvs_set_u8(h,  "pmesh",    cfg->pseudo_mesh);
    nvs_set_u8(h,  "roam_rssi", (uint8_t)cfg->roam_rssi_threshold);
    nvs_set_u8(h,  "roam_hyst", cfg->roam_hysteresis);
    nvs_set_u8(h,  "band_mode", cfg->band_mode);
    nvs_set_u8(h,  "bw_2g",     cfg->bw_2g);
    nvs_set_u8(h,  "bw_5g",     cfg->bw_5g);
    nvs_set_u8(h,  "adj5g",     cfg->rssi_5g_adj);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved to NVS");
    return err;
}

esp_err_t repeater_config_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Config reset to defaults");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  PHY guard — ochrona przed zapętleniem na złej konfiguracji radia
 *
 *  Zła kombinacja pasma / protokołu / bandwidth potrafi zawiesić CPU
 *  tak, że nie da się nawet przeflashować płytki bez BOOT+RESET.
 *
 *  Dlatego przed dotknięciem PHY zapisujemy w NVS flagę "próbuję",
 *  a po udanej konfiguracji ją czyścimy. Jeśli przy starcie flaga
 *  wciąż jest ustawiona, znaczy że poprzedni boot nie doszedł do
 *  końca — wtedy pomijamy konfigurację PHY i wstajemy na bezpiecznych
 *  ustawieniach (2.4 GHz, 20 MHz). Wystarczy odłączyć i podłączyć
 *  zasilanie, żeby odzyskać dostęp do GUI i zmienić ustawienia.
 * ══════════════════════════════════════════════════════════════ */

#define PHY_GUARD_KEY "phy_try"

bool repeater_phy_guard_tripped(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, PHY_GUARD_KEY, &v);
    nvs_close(h);
    return (err == ESP_OK && v != 0);
}

static void phy_guard_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, PHY_GUARD_KEY, v);
    nvs_commit(h);
    nvs_close(h);
}

void repeater_phy_guard_arm(void)   { phy_guard_set(1); }
void repeater_phy_guard_disarm(void) { phy_guard_set(0); }

/* ══════════════════════════════════════════════════════════════
 *  Boot stage marker — diagnostyka bez UART
 *
 *  Na ESP32-C5 z USB-Serial/JTAG bywa, że logu bootu nie da się złapać
 *  (reset po liniach CDC wchodzi w download mode, a przy zawieszeniu
 *  endpoint się zapycha). Dlatego każdy etap startu zapisuje numer do
 *  NVS. Po BOOT+RESET można odczytać partycję NVS z hosta:
 *
 *      python tools\read_boot_stage.py
 *
 *  i zobaczyć, na którym etapie firmware przestał się posuwać.
 * ══════════════════════════════════════════════════════════════ */

#define BOOT_STAGE_KEY "bootstg"

void repeater_boot_stage_set(uint8_t stage)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, BOOT_STAGE_KEY, stage);
    nvs_commit(h);
    nvs_close(h);
}


