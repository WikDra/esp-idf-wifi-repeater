/*
 * repeater_config.c — NVS-backed runtime configuration
 */
#include <string.h>
#include "repeater_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "rep_cfg";
#define NVS_NAMESPACE "rep_cfg"

/* ── helpers ─────────────────────────────────────────────────── */

static void load_str(nvs_handle_t h, const char *key,
                     char *dst, size_t dst_sz, const char *def)
{
    size_t len = dst_sz;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, def, dst_sz);
    }
}

static void load_u8(nvs_handle_t h, const char *key,
                    uint8_t *dst, uint8_t def)
{
    if (nvs_get_u8(h, key, dst) != ESP_OK) {
        *dst = def;
    }
}

/* ── public API ──────────────────────────────────────────────── */

esp_err_t repeater_config_load(repeater_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* No saved config — use Kconfig defaults */
        ESP_LOGI(TAG, "No NVS config, using menuconfig defaults");
        strlcpy(cfg->sta_ssid, CONFIG_REPEATER_UPSTREAM_SSID, sizeof(cfg->sta_ssid));
        strlcpy(cfg->sta_pass, CONFIG_REPEATER_UPSTREAM_PASSWORD, sizeof(cfg->sta_pass));
        strlcpy(cfg->ap_ssid,  CONFIG_REPEATER_AP_SSID,  sizeof(cfg->ap_ssid));
        strlcpy(cfg->ap_pass,  CONFIG_REPEATER_AP_PASSWORD, sizeof(cfg->ap_pass));
        cfg->tx_power_dbm = CONFIG_REPEATER_TX_POWER;
        cfg->max_clients  = CONFIG_REPEATER_MAX_CLIENTS;
        return ESP_OK;
    }
    if (err != ESP_OK) return err;

    ESP_LOGI(TAG, "Loading config from NVS");
    load_str(h, "sta_ssid", cfg->sta_ssid, sizeof(cfg->sta_ssid), CONFIG_REPEATER_UPSTREAM_SSID);
    load_str(h, "sta_pass", cfg->sta_pass, sizeof(cfg->sta_pass), CONFIG_REPEATER_UPSTREAM_PASSWORD);
    load_str(h, "ap_ssid",  cfg->ap_ssid,  sizeof(cfg->ap_ssid),  CONFIG_REPEATER_AP_SSID);
    load_str(h, "ap_pass",  cfg->ap_pass,  sizeof(cfg->ap_pass),  CONFIG_REPEATER_AP_PASSWORD);
    load_u8(h, "tx_power", &cfg->tx_power_dbm, CONFIG_REPEATER_TX_POWER);
    load_u8(h, "max_cli",  &cfg->max_clients,  CONFIG_REPEATER_MAX_CLIENTS);

    nvs_close(h);
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
