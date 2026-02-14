/*
 * ESP32-C6 WiFi 6 Repeater (bez NAT, ta sama podsieć)
 *
 * Architektura:
 *   ESP32-C6 działa w trybie APSTA (jednoczesne STA + SoftAP).
 *   STA łączy się z upstream AP (router). AP tworzy sieć dla klientów.
 *   Pakiety są bridgowane na warstwie L2 między interfejsami.
 *
 * Kluczowy mechanizm — MAC cloning:
 *   Gdy klient łączy się z naszym AP, repeater:
 *     1. Rozłącza STA z upstream AP
 *     2. Zmienia MAC adres STA na MAC klienta (esp_wifi_set_mac)
 *     3. Łączy się ponownie z upstream AP
 *     4. Wyłącza DHCP client na STA (żeby nie kolidował z klientem)
 *   Dzięki temu upstream AP widzi klienta bezpośrednio.
 *   DHCP, ARP, wszystko działa natywnie — ta sama podsieć, zero NAT.
 *
 *   Gdy klient się rozłącza:
 *     1. Przywraca oryginalny MAC na STA
 *     2. Reconnect z upstream, włącza z powrotem DHCP client
 *
 * Packet forwarding:
 *   esp_wifi_internal_reg_rxcb() przechwytuje pakiety L2 ZANIM
 *   trafią do stosu TCP/IP. Callback zastępuje domyślny handler.
 *   - STA rx → forward do AP (do klienta)
 *   - AP rx  → forward do STA (upstream)
 *
 * Ograniczenie: w trybie MAC cloning obsługujemy jednego klienta
 * (bo STA może mieć tylko jeden MAC). Dla wielu klientów potrzebny
 * byłby WDS/4-addr mode, którego ESP32 nie wspiera.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "wifi6_rep";

/* ── Event bits ─────────────────────────────────────────────── */
#define STA_CONNECTED_BIT   BIT0
#define STA_DISCONNECTED_BIT BIT1
static EventGroupHandle_t s_wifi_event_group;

/* ── MAC adresy ─────────────────────────────────────────────── */
static uint8_t s_original_sta_mac[6];   /* oryginalny MAC STA (fabryczny) */
static uint8_t s_ap_mac[6];             /* MAC naszego AP */
static uint8_t s_client_mac[6];         /* MAC aktualnie podłączonego klienta */
static uint8_t s_upstream_bssid[6];      /* BSSID upstream AP do którego się łączymy */
static uint8_t s_upstream_channel;       /* kanał upstream AP */
static bool    s_bssid_locked = false;   /* czy mamy zapisany BSSID */

/* ── Stan ───────────────────────────────────────────────────── */
typedef enum {
    STATE_IDLE,              /* STA connected z własnym MAC, brak klientów */
    STATE_MAC_CHANGING,      /* trwa zmiana MAC (disconnect→change→reconnect) */
    STATE_BRIDGING,          /* bridge aktywny, STA MAC = client MAC */
    STATE_MAC_RESTORING,     /* przywracanie oryginalnego MAC */
} repeater_state_t;

static volatile repeater_state_t s_state = STATE_IDLE;
static volatile bool s_sta_connected = false;
static volatile bool s_forwarding_active = false;
static volatile bool s_mac_cloned = false;
static volatile bool s_suppress_auto_reconnect = false;  /* blokuj auto-reconnect podczas zmiany MAC */

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static TaskHandle_t s_mac_task_handle = NULL;
static SemaphoreHandle_t s_mac_task_mutex;   /* zapobiega równoległym zmianom MAC */

/* ══════════════════════════════════════════════════════════════
 *  L2 Packet Forwarding
 *
 *  esp_wifi_internal_reg_rxcb() ZASTĘPUJE domyślny handler.
 *  Po rejestracji, pakiety NIE trafiają do lwIP automatycznie.
 *
 *  W trybie bridging (STA MAC = client MAC):
 *    - STA rx: forward do AP (do klienta) + do lwIP tylko broadcast
 *    - AP rx:  forward do STA (upstream)
 *
 *  W trybie idle (STA z własnym MAC):
 *    - Normalna praca, forwarding wyłączony
 * ══════════════════════════════════════════════════════════════ */

static esp_err_t on_sta_rx(void *buffer, uint16_t len, void *eb)
{
    if (!buffer || len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }

    uint8_t *dst = (uint8_t *)buffer;

    /* Forward WSZYSTKO do klienta na AP */
    esp_wifi_internal_tx(WIFI_IF_AP, buffer, len);

    /* Broadcast/multicast: podaj też do naszego stosu lwIP
     * (np. ARP, mDNS — przydatne dla diagnostyki) */
    if (dst[0] & 0x01) {
        esp_netif_receive(s_sta_netif, buffer, len, eb);
        return ESP_OK;
    }

    /* Unicast: tylko forward, nie dawaj do stosu (DHCP client wyłączony) */
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static esp_err_t on_ap_rx(void *buffer, uint16_t len, void *eb)
{
    if (!buffer || len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }

    /* Forward WSZYSTKO upstream przez STA */
    if (s_sta_connected) {
        esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
    }

    /* Nie dawaj do stosu AP (DHCP server wyłączony, AP nie potrzebuje przetwarzać) */
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static void forwarding_start(void)
{
    if (s_forwarding_active) return;
    ESP_LOGI(TAG, ">>> Forwarding START");
    /* Wyłącz power save — minimalna latencja podczas bridgowania */
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, on_sta_rx);
    esp_wifi_internal_reg_rxcb(WIFI_IF_AP, on_ap_rx);
    s_forwarding_active = true;
}

static void forwarding_stop(void)
{
    if (!s_forwarding_active) return;
    ESP_LOGI(TAG, "<<< Forwarding STOP");
    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
    esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
    s_forwarding_active = false;
    /* Przywróć modem sleep w trybie idle */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

/* ══════════════════════════════════════════════════════════════
 *  MAC clone task
 *
 *  Wykonuje operacje zmiany MAC w osobnym tasku (nie w event handlerze),
 *  bo wymaga disconnect/reconnect co generuje nowe eventy.
 * ══════════════════════════════════════════════════════════════ */

/* Parametry przekazywane do tasku */
typedef struct {
    uint8_t mac[6];
    bool    clone;     /* true = clone client MAC, false = restore original */
} mac_task_params_t;

static void mac_change_task(void *pvParams)
{
    mac_task_params_t *params = (mac_task_params_t *)pvParams;

    /* Zablokuj równoległe zmiany */
    if (xSemaphoreTake(s_mac_task_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "MAC change already in progress, skipping");
        free(params);
        vTaskDelete(NULL);
        return;
    }

    if (params->clone) {
        /* ── Clone client MAC ─────────────────── */
        s_state = STATE_MAC_CHANGING;
        ESP_LOGI(TAG, "=== MAC CLONE: " MACSTR " ===", MAC2STR(params->mac));

        /* 1. Stop forwarding */
        forwarding_stop();

        /* 2. Suppress auto-reconnect in event handler */
        s_suppress_auto_reconnect = true;

        /* 3. Disconnect STA */
        ESP_LOGI(TAG, "  Disconnecting STA...");
        esp_wifi_disconnect();

        /* 4. Czekaj na disconnect */
        xEventGroupWaitBits(s_wifi_event_group, STA_DISCONNECTED_BIT,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        vTaskDelay(pdMS_TO_TICKS(100));

        /* 5. Wyłącz DHCP client na STA
         *    (żeby nie kolidował z DHCP klienta — oba mają ten sam MAC) */
        esp_netif_dhcpc_stop(s_sta_netif);
        /* Ustaw dummy static IP żeby esp_netif_handlers nie narzekał "invalid static ip" */
        {
            esp_netif_ip_info_t dummy_ip = {
                .ip      = { .addr = ESP_IP4TOADDR(169, 254, 1, 1) },
                .netmask = { .addr = ESP_IP4TOADDR(255, 255, 0, 0) },
                .gw      = { .addr = ESP_IP4TOADDR(0, 0, 0, 0) },
            };
            esp_netif_set_ip_info(s_sta_netif, &dummy_ip);
        }
        ESP_LOGI(TAG, "  DHCP client stopped on STA");

        /* 6. Zmień MAC na STA */
        esp_err_t err = esp_wifi_set_mac(WIFI_IF_STA, params->mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "  esp_wifi_set_mac failed: %s", esp_err_to_name(err));
            /* Fallback: przywróć oryginał i reconnect */
            esp_wifi_set_mac(WIFI_IF_STA, s_original_sta_mac);
            s_suppress_auto_reconnect = false;
            esp_wifi_connect();
            xSemaphoreGive(s_mac_task_mutex);
            free(params);
            vTaskDelete(NULL);
            return;
        }

        /* Weryfikacja */
        uint8_t verify_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, verify_mac);
        ESP_LOGI(TAG, "  STA MAC now: " MACSTR, MAC2STR(verify_mac));

        s_mac_cloned = true;

        /* 7. Reconnect z nowym MAC — użyj zapisanego BSSID żeby nie skakać po kanałach */
        ESP_LOGI(TAG, "  Reconnecting with cloned MAC...");
        if (s_bssid_locked) {
            wifi_config_t current_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &current_cfg);
            memcpy(current_cfg.sta.bssid, s_upstream_bssid, 6);
            current_cfg.sta.bssid_set = true;
            current_cfg.sta.channel = s_upstream_channel;
            esp_wifi_set_config(WIFI_IF_STA, &current_cfg);
            ESP_LOGI(TAG, "  BSSID locked to: " MACSTR " ch %d",
                     MAC2STR(s_upstream_bssid), s_upstream_channel);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        s_suppress_auto_reconnect = false;
        esp_wifi_connect();

        /* 8. Czekaj na połączenie */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, STA_CONNECTED_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & STA_CONNECTED_BIT) {
            ESP_LOGI(TAG, "=== BRIDGE ACTIVE ===");
            s_state = STATE_BRIDGING;
            /* Forwarding jest uruchamiany w STA_CONNECTED handlerze */
        } else {
            ESP_LOGE(TAG, "  Reconnect timeout! Restoring original MAC...");
            s_suppress_auto_reconnect = true;
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_wifi_set_mac(WIFI_IF_STA, s_original_sta_mac);
            s_mac_cloned = false;
            esp_netif_dhcpc_start(s_sta_netif);
            /* Odblokuj BSSID — pozwól na pełny scan przy fallback */
            {
                wifi_config_t current_cfg;
                esp_wifi_get_config(WIFI_IF_STA, &current_cfg);
                current_cfg.sta.bssid_set = false;
                current_cfg.sta.channel = 0;
                esp_wifi_set_config(WIFI_IF_STA, &current_cfg);
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            s_suppress_auto_reconnect = false;
            esp_wifi_connect();
            s_state = STATE_IDLE;
        }

    } else {
        /* ── Restore original MAC ─────────────── */
        s_state = STATE_MAC_RESTORING;
        ESP_LOGI(TAG, "=== MAC RESTORE ===");

        /* 1. Stop forwarding */
        forwarding_stop();

        /* 2. Suppress auto-reconnect */
        s_suppress_auto_reconnect = true;

        /* 3. Disconnect */
        ESP_LOGI(TAG, "  Disconnecting STA...");
        esp_wifi_disconnect();

        xEventGroupWaitBits(s_wifi_event_group, STA_DISCONNECTED_BIT,
                            pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
        vTaskDelay(pdMS_TO_TICKS(100));

        /* 4. Przywróć oryginalny MAC */
        esp_wifi_set_mac(WIFI_IF_STA, s_original_sta_mac);
        s_mac_cloned = false;

        uint8_t verify_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, verify_mac);
        ESP_LOGI(TAG, "  STA MAC restored: " MACSTR, MAC2STR(verify_mac));

        /* 5. Włącz z powrotem DHCP client */
        esp_netif_dhcpc_start(s_sta_netif);
        ESP_LOGI(TAG, "  DHCP client re-enabled");

        /* 6. Reconnect — odblokuj BSSID, pozwól na pełny scan */
        {
            wifi_config_t current_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &current_cfg);
            current_cfg.sta.bssid_set = false;
            current_cfg.sta.channel = 0;
            esp_wifi_set_config(WIFI_IF_STA, &current_cfg);
        }
        ESP_LOGI(TAG, "  Reconnecting with original MAC...");
        vTaskDelay(pdMS_TO_TICKS(200));
        s_suppress_auto_reconnect = false;
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, STA_CONNECTED_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & STA_CONNECTED_BIT) {
            ESP_LOGI(TAG, "=== IDLE MODE (own IP) ===");
        } else {
            ESP_LOGW(TAG, "  Reconnect timeout, will retry automatically");
        }
        s_state = STATE_IDLE;
    }

    xSemaphoreGive(s_mac_task_mutex);
    s_mac_task_handle = NULL;
    free(params);
    vTaskDelete(NULL);
}

static void request_mac_clone(const uint8_t *client_mac)
{
    mac_task_params_t *params = malloc(sizeof(mac_task_params_t));
    if (!params) return;
    memcpy(params->mac, client_mac, 6);
    params->clone = true;
    xTaskCreate(mac_change_task, "mac_clone", 4096, params, 10, &s_mac_task_handle);
}

static void request_mac_restore(void)
{
    mac_task_params_t *params = malloc(sizeof(mac_task_params_t));
    if (!params) return;
    memcpy(params->mac, s_original_sta_mac, 6);
    params->clone = false;
    xTaskCreate(mac_change_task, "mac_restore", 4096, params, 10, &s_mac_task_handle);
}

/* ══════════════════════════════════════════════════════════════
 *  WiFi event handlers
 * ══════════════════════════════════════════════════════════════ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    switch (id) {

    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        /* Nie łącz jeśli mac_change_task sam zarządza połączeniem */
        if (!s_suppress_auto_reconnect) {
            ESP_LOGI(TAG, "  Auto-connecting...");
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_CONNECTED: {
        wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *)data;
        ESP_LOGI(TAG, ">> Connected to: %.*s (ch %d, BSSID " MACSTR ")",
                 ev->ssid_len, ev->ssid, ev->channel, MAC2STR(ev->bssid));
        s_sta_connected = true;
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_event_group, STA_DISCONNECTED_BIT);

        /* Zapamiętaj BSSID i kanał upstream AP żeby przy reconnect nie skakać po kanałach */
        if (!s_bssid_locked) {
            memcpy(s_upstream_bssid, ev->bssid, 6);
            s_upstream_channel = ev->channel;
            s_bssid_locked = true;
            ESP_LOGI(TAG, "  BSSID locked: " MACSTR " ch %d",
                     MAC2STR(s_upstream_bssid), s_upstream_channel);
        }

        /* Jeśli jesteśmy w trybie bridging (MAC cloned), włącz forwarding */
        if (s_mac_cloned) {
            forwarding_start();
        }
        break;
    }

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "<< Disconnected (reason %d)", ev->reason);
        s_sta_connected = false;
        xEventGroupSetBits(s_wifi_event_group, STA_DISCONNECTED_BIT);
        xEventGroupClearBits(s_wifi_event_group, STA_CONNECTED_BIT);

        forwarding_stop();

        /* Auto-reconnect, ale NIE gdy mac_change_task sam zarządza połączeniem */
        if (!s_suppress_auto_reconnect) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "Auto-reconnecting...");
            esp_wifi_connect();
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "-> Client joined: " MACSTR " (AID=%d)",
                 MAC2STR(ev->mac), ev->aid);

        /* Jeśli jesteśmy w trybie IDLE (brak klona) → klonuj MAC klienta */
        if (s_state == STATE_IDLE && !s_mac_cloned) {
            memcpy(s_client_mac, ev->mac, 6);
            request_mac_clone(ev->mac);
        } else if (s_mac_cloned) {
            /* Już mamy klienta i bridge aktywny.
             * Drugi klient nie będzie w pełni obsługiwany
             * (single-client MAC clone limitation). */
            ESP_LOGW(TAG, "Bridge already active for " MACSTR
                     ". Additional client may not get upstream access.",
                     MAC2STR(s_client_mac));
        }
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "<- Client left: " MACSTR " (AID=%d)",
                 MAC2STR(ev->mac), ev->aid);

        /* Jeśli odszedł ten klient, dla którego klonowaliśmy MAC → przywróć */
        if (s_mac_cloned && memcmp(ev->mac, s_client_mac, 6) == 0) {
            /* Sprawdź ile klientów zostało (odfiltruj odchodzącego — race condition) */
            wifi_sta_list_t sta_list;
            int remaining = 0;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
                for (int i = 0; i < sta_list.num; i++) {
                    if (memcmp(sta_list.sta[i].mac, ev->mac, 6) != 0) {
                        remaining++;
                    }
                }
            }

            if (remaining == 0) {
                ESP_LOGI(TAG, "Last client left, restoring MAC...");
                request_mac_restore();
            } else {
                /* Są jeszcze inni klienci, ale MAC jest sklonowany
                 * pod starego klienta. Sklonuj pod pierwszego dostępnego. */
                ESP_LOGI(TAG, "Cloned client left, but %d other clients remain. "
                         "Re-cloning for first available...", remaining);
                for (int i = 0; i < sta_list.num; i++) {
                    if (memcmp(sta_list.sta[i].mac, ev->mac, 6) != 0) {
                        memcpy(s_client_mac, sta_list.sta[i].mac, 6);
                        request_mac_clone(sta_list.sta[i].mac);
                        break;
                    }
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "=== Got IP: " IPSTR " gw: " IPSTR " ===",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  WiFi 6 info + status
 * ══════════════════════════════════════════════════════════════ */

static void print_wifi6_info(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== WiFi 6 (802.11ax) Repeater ===");
#if SOC_WIFI_HE_SUPPORT
    ESP_LOGI(TAG, "  HE (High Efficiency): CAPABLE");
    ESP_LOGI(TAG, "  OFDMA / BSS Coloring: CAPABLE");
    ESP_LOGI(TAG, "  MCS 0-9:              YES");
    ESP_LOGI(TAG, "  (WiFi6 only if upstream AP supports it)");
#else
    ESP_LOGW(TAG, "  HE not available on this SoC");
#endif
    ESP_LOGI(TAG, "  Compat: WiFi 4/5");
    ESP_LOGI(TAG, "  BW: HT20 (for HE), Security: WPA2/WPA3");
    ESP_LOGI(TAG, "===================================");
}

static void status_task(void *pv)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        const char *state_str;
        switch (s_state) {
            case STATE_IDLE:          state_str = "IDLE"; break;
            case STATE_MAC_CHANGING:  state_str = "MAC_CHANGING"; break;
            case STATE_BRIDGING:      state_str = "BRIDGING"; break;
            case STATE_MAC_RESTORING: state_str = "MAC_RESTORING"; break;
            default:                  state_str = "UNKNOWN"; break;
        }

        ESP_LOGI(TAG, "--- Status [%s] ---", state_str);

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "  Up: %s RSSI:%d Ch:%d", ap.ssid, ap.rssi, ap.primary);
#if SOC_WIFI_HE_SUPPORT
            ESP_LOGI(TAG, "  PHY: %s",
                     ap.phy_11ax ? "WiFi6(11ax)" :
                     ap.phy_11n  ? "WiFi4(11n)"  : "Legacy");
#endif
        } else {
            ESP_LOGW(TAG, "  Up: not connected");
        }

        uint8_t current_mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, current_mac);
        ESP_LOGI(TAG, "  STA MAC: " MACSTR " %s",
                 MAC2STR(current_mac),
                 s_mac_cloned ? "(CLONED)" : "(original)");

        wifi_sta_list_t sta_list;
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            ESP_LOGI(TAG, "  Clients: %d", sta_list.num);
            for (int i = 0; i < sta_list.num; i++) {
                ESP_LOGI(TAG, "    [%d] " MACSTR " RSSI:%d",
                         i + 1, MAC2STR(sta_list.sta[i].mac), sta_list.sta[i].rssi);
            }
        }
        ESP_LOGI(TAG, "  Forwarding: %s", s_forwarding_active ? "ON" : "OFF");
        ESP_LOGI(TAG, "---");
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Inicjalizacja WiFi
 * ══════════════════════════════════════════════════════════════ */

static void init_wifi(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    assert(s_sta_netif && s_ap_netif);

    /* Wyłącz DHCP server na AP — klienci dostaną IP z upstream DHCP */
    esp_netif_dhcps_stop(s_ap_netif);

    /* Wyłącz też IP na AP (nie potrzebuje 192.168.4.x) */
    esp_netif_ip_info_t ip_info = { 0 };
    esp_netif_set_ip_info(s_ap_netif, &ip_info);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Zachowaj oryginalny MAC */
    esp_read_mac(s_original_sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(s_ap_mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(s_original_sta_mac));
    ESP_LOGI(TAG, "AP  MAC: " MACSTR, MAC2STR(s_ap_mac));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* STA config */
    wifi_config_t sta_cfg = {
        .sta = {
            .ssid = CONFIG_REPEATER_UPSTREAM_SSID,
            .password = CONFIG_REPEATER_UPSTREAM_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#if SOC_WIFI_HE_SUPPORT
            .he_dcm_set = 0,
            .he_dcm_max_constellation_tx = 2,
            .he_dcm_max_constellation_rx = 2,
            .he_mcs9_enabled = 1,
#endif
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* AP config */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = CONFIG_REPEATER_AP_SSID,
            .ssid_len = strlen(CONFIG_REPEATER_AP_SSID),
            .password = CONFIG_REPEATER_AP_PASSWORD,
            .channel = 0,
            .max_connection = CONFIG_REPEATER_MAX_CLIENTS,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .pmf_cfg = { .required = false, .capable = true },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    if (strlen(CONFIG_REPEATER_AP_PASSWORD) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* HT20 na 2.4 GHz — HT40 wymusza fallback do 11n (WiFi 4).
     * WiFi 6 (11ax) na C6 działa tylko z HT20.
     * Przepustowość HE HT20 MCS9 ≈ 143 Mbps — porównywalnie z HT40 11n. */
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(CONFIG_REPEATER_TX_POWER * 4);

    /* Event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL));
}

/* ══════════════════════════════════════════════════════════════
 *  app_main
 * ══════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-C6 WiFi 6 Repeater (no NAT)");
    ESP_LOGI(TAG, "  L2 Bridge - MAC Cloning");
    ESP_LOGI(TAG, "========================================");

    s_wifi_event_group = xEventGroupCreate();
    s_mac_task_mutex = xSemaphoreCreateMutex();

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    print_wifi6_info();
    init_wifi();

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "APSTA started");
    ESP_LOGI(TAG, "  Upstream: %s", CONFIG_REPEATER_UPSTREAM_SSID);
    ESP_LOGI(TAG, "  Repeater: %s", CONFIG_REPEATER_AP_SSID);

    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Waiting for connections...");
}
