/*
 * ESP32 WiFi Repeater (bez NAT, ta sama podsieć)
 *
 * Obsługiwane SoC: ESP32-C6 (WiFi 6), ESP32-S3 (WiFi 5)
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
#include "lwip/inet.h"
#include "repeater_config.h"
#include "repeater_httpd.h"

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

/* Non-static: accessed by repeater_httpd.c via extern for /status endpoint */
volatile repeater_state_t s_state = STATE_IDLE;
volatile bool s_sta_connected = false;
volatile bool s_forwarding_active = false;
volatile bool s_mac_cloned = false;
static volatile bool s_suppress_auto_reconnect = false;  /* blokuj auto-reconnect podczas zmiany MAC */

esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static int s_client_count = 0;           /* ile klientów podłączonych do AP */
static bool s_ap_ip_from_sniff = false;  /* AP IP ustawione z DHCP sniffera */

/* Runtime config loaded from NVS (or menuconfig defaults) */
static repeater_config_t s_cfg;

static TaskHandle_t s_mac_task_handle = NULL;
static SemaphoreHandle_t s_mac_task_mutex;   /* zapobiega równoległym zmianom MAC */

/* Forward declarations */
static void ap_mirror_sta_ip(const esp_netif_ip_info_t *sta_ip);
static void ap_restore_management_ip(void);
static void sniff_dhcp_ack_and_set_ap_ip(const uint8_t *data, uint16_t len);
static void macnat_rewrite_upstream(uint8_t *frame, uint16_t len);
static void macnat_rewrite_downstream(uint8_t *frame, uint16_t len);
static void macnat_learn(uint32_t ip_n, const uint8_t *mac);

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

    /* Sniff DHCP ACK — only if UDP port 67→68 (skip 99.9% packets with inline check) */
    if (len >= 286 && dst[12] == 0x08 && dst[13] == 0x00) {
        const uint8_t *ip_hdr = dst + 14;
        if (ip_hdr[9] == 17) {  /* UDP */
            uint8_t ihl = (ip_hdr[0] & 0x0F) * 4;
            const uint8_t *udp = ip_hdr + ihl;
            if (14 + ihl + 8 <= len && udp[0] == 0 && udp[1] == 67 && udp[2] == 0 && udp[3] == 68) {
                sniff_dhcp_ack_and_set_ap_ip(dst, len);
            }
        }
    }

    /* MAC-NAT downstream: przepisz dst MAC dla dodatkowych klientów
     * Skip jeśli jest tylko 1 klient (primary) — nic do przepisywania */
    if (s_client_count > 1 && !(dst[0] & 0x01)) {
        macnat_rewrite_downstream((uint8_t *)buffer, len);
    }

    /* Forward WSZYSTKO do klienta na AP */
    esp_wifi_internal_tx(WIFI_IF_AP, buffer, len);

    /* Broadcast/multicast: podaj też do naszego stosu lwIP
     * (np. ARP, mDNS — przydatne dla diagnostyki) */
    if (dst[0] & 0x01) {
        esp_netif_receive(s_sta_netif, buffer, len, eb);
        return ESP_OK;
    }

    /* Unicast do NASZEGO MAC (STA) — podaj do stosu lwIP
     * (HTTP config GUI, ping, itp. z upstream sieci) */
    if (memcmp(dst, s_original_sta_mac, 6) == 0 ||
        memcmp(dst, s_client_mac, 6) == 0) {
        esp_netif_receive(s_sta_netif, buffer, len, eb);
        return ESP_OK;
    }

    /* Unicast do klienta: tylko forward */
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

static esp_err_t on_ap_rx(void *buffer, uint16_t len, void *eb)
{
    if (!buffer || len < 14) {
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_OK;
    }

    uint8_t *dst = (uint8_t *)buffer;
    uint8_t *src = (uint8_t *)buffer + 6;

    /* MAC-NAT upstream: przepisz src MAC non-primary klientów
     * Skip jeśli jest tylko 1 klient */
    if (s_client_count > 1 && !(src[0] & 0x01) &&
        memcmp(src, s_client_mac, 6) != 0) {
        macnat_rewrite_upstream((uint8_t *)buffer, len);
    }

    /* Broadcast/multicast — forward upstream + podaj do stosu AP */
    if (dst[0] & 0x01) {
        if (s_sta_connected) {
            esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
        }
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }

    /* Unicast do NASZEGO MAC (AP) — podaj do stosu lwIP
     * (HTTP config GUI pod 192.168.4.1, ARP, itp.) */
    if (memcmp(dst, s_ap_mac, 6) == 0) {
        esp_netif_receive(s_ap_netif, buffer, len, eb);
        return ESP_OK;
    }

    /* Unicast do upstream — forward przez STA */
    if (s_sta_connected) {
        esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
    }

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
 *  MAC-NAT: Multi-client support
 *
 *  STA ma MAC sklonowany pod jednego klienta (primary). Dodatkowi
 *  klienci nie byliby widziani przez router (802.11 TA != ich MAC).
 *
 *  Rozwiązanie:
 *   Upstream (AP→STA): przepisz src MAC dodatkowych klientów na
 *                      sklonowany MAC. Router widzi jeden MAC.
 *   Downstream (STA→AP): sprawdź dst IP w tablicy IP→MAC,
 *                         przepisz dst MAC na prawdziwy MAC klienta.
 *
 *  Tablica IP→MAC uczona z pakietów klientów (IPv4 src, ARP sender)
 *  i z DHCP ACK (yiaddr→chaddr).
 * ══════════════════════════════════════════════════════════════ */

#define MACNAT_MAX 8

typedef struct {
    uint32_t ip;          /* network byte order */
    uint8_t  real_mac[6]; /* prawdziwy MAC klienta */
    int64_t  last_seen;   /* esp_timer_get_time() timestamp */
    bool     used;
} macnat_entry_t;

static macnat_entry_t s_macnat[MACNAT_MAX];

static void macnat_learn(uint32_t ip_n, const uint8_t *mac)
{
    /* Ignoruj broadcast/multicast MAC i zerowy IP */
    if ((mac[0] & 0x01) || ip_n == 0) return;

    int free_idx = -1;
    int oldest_idx = 0;

    for (int i = 0; i < MACNAT_MAX; i++) {
        if (s_macnat[i].used) {
            /* Hot path: istniejący wpis, ten sam IP+MAC — nic nie rób */
            if (s_macnat[i].ip == ip_n) {
                if (memcmp(s_macnat[i].real_mac, mac, 6) == 0) return;
                /* IP istnieje ale MAC się zmienił */
                memcpy(s_macnat[i].real_mac, mac, 6);
                s_macnat[i].last_seen = esp_timer_get_time();
                return;
            }
            if (memcmp(s_macnat[i].real_mac, mac, 6) == 0) {
                /* Ten sam MAC, nowe IP (DHCP renewal) */
                s_macnat[i].ip = ip_n;
                s_macnat[i].last_seen = esp_timer_get_time();
                return;
            }
            if (s_macnat[i].last_seen < s_macnat[oldest_idx].last_seen) {
                oldest_idx = i;
            }
        } else if (free_idx == -1) {
            free_idx = i;
        }
    }

    /* Nowy wpis */
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    s_macnat[idx].ip = ip_n;
    memcpy(s_macnat[idx].real_mac, mac, 6);
    s_macnat[idx].last_seen = esp_timer_get_time();
    s_macnat[idx].used = true;
    ESP_LOGI(TAG, "MAC-NAT learned: " IPSTR " -> " MACSTR,
             IP2STR((esp_ip4_addr_t *)&ip_n), MAC2STR(mac));
}

static const uint8_t *macnat_lookup_by_ip(uint32_t ip_n)
{
    for (int i = 0; i < MACNAT_MAX; i++) {
        if (s_macnat[i].used && s_macnat[i].ip == ip_n) {
            return s_macnat[i].real_mac;
        }
    }
    return NULL;
}

static void macnat_clear(void)
{
    memset(s_macnat, 0, sizeof(s_macnat));
}

/* Upstream: przepisz src MAC dodatkowego klienta na sklonowany MAC.
 * Router widzi jeden MAC, a my zapamiętujemy IP→MAC do powrotu. */
static void macnat_rewrite_upstream(uint8_t *frame, uint16_t len)
{
    uint8_t *eth_src = frame + 6;
    uint16_t ethertype = (frame[12] << 8) | frame[13];

    if (ethertype == 0x0800 && len >= 34) {
        /* IPv4: src IP at offset 26 */
        uint32_t src_ip;
        memcpy(&src_ip, frame + 26, 4);
        macnat_learn(src_ip, eth_src);

        /* DHCP fix: klient wysyła Discover/Request z chaddr = swój MAC.
         * Router odpowiada unicast do chaddr → WiFi HW na STA odrzuca
         * (STA MAC = sklonowany ≠ chaddr). Fix: ustaw BROADCAST flag
         * w DHCP, żeby router odpowiedział broadcastem. */
        const uint8_t *ip_hdr = frame + 14;
        if (ip_hdr[9] == 17) {  /* UDP */
            uint8_t ihl = (ip_hdr[0] & 0x0F) * 4;
            const uint8_t *udp = ip_hdr + ihl;
            if (14 + ihl + 8 <= len &&
                udp[0] == 0 && udp[1] == 68 &&   /* src port 68 (DHCP client) */
                udp[2] == 0 && udp[3] == 67) {    /* dst port 67 (DHCP server) */
                uint8_t *dhcp = (uint8_t *)(udp + 8);
                int dhcp_off = 14 + ihl + 8;
                if (dhcp_off + 44 <= len) {
                    /* Set BROADCAST flag (bit 15 of flags field at DHCP offset 10)
                     * Forces server to respond via broadcast instead of unicast to chaddr */
                    dhcp[10] |= 0x80;
                    /* Zero UDP checksum — modifying payload invalidates it.
                     * UDP/IPv4 allows checksum=0 meaning "not computed" (RFC 768). */
                    uint8_t *udp_csum = (uint8_t *)(udp + 6);
                    udp_csum[0] = 0;
                    udp_csum[1] = 0;
                    ESP_LOGD(TAG, "MAC-NAT: set BROADCAST flag in DHCP from " MACSTR,
                             MAC2STR(eth_src));
                }
            }
        }
    } else if (ethertype == 0x0806 && len >= 42) {
        /* ARP: sender IP at 28, sender MAC at 22 */
        uint32_t sender_ip;
        memcpy(&sender_ip, frame + 28, 4);
        macnat_learn(sender_ip, eth_src);
        /* Przepisz ARP sender hardware address */
        memcpy(frame + 22, s_client_mac, 6);
    }

    /* Przepisz Ethernet source MAC */
    memcpy(eth_src, s_client_mac, 6);
}

/* Downstream: przepisz dst MAC ze sklonowanego na prawdziwy MAC klienta.
 * Router wysyła do sklonowanego MAC — my podmieniamy na docelowy. */
static void macnat_rewrite_downstream(uint8_t *frame, uint16_t len)
{
    uint16_t ethertype = (frame[12] << 8) | frame[13];
    const uint8_t *real_mac = NULL;

    if (ethertype == 0x0800 && len >= 34) {
        /* IPv4: dst IP at offset 30 */
        uint32_t dst_ip;
        memcpy(&dst_ip, frame + 30, 4);
        real_mac = macnat_lookup_by_ip(dst_ip);
    } else if (ethertype == 0x0806 && len >= 42) {
        /* ARP: target IP at 38, target MAC at 32 */
        uint32_t target_ip;
        memcpy(&target_ip, frame + 38, 4);
        real_mac = macnat_lookup_by_ip(target_ip);
        if (real_mac && memcmp(real_mac, s_client_mac, 6) != 0) {
            /* Przepisz ARP target hardware address */
            memcpy(frame + 32, real_mac, 6);
        }
    }

    /* Przepisz Ethernet dst MAC tylko dla dodatkowych klientów */
    if (real_mac && memcmp(real_mac, s_client_mac, 6) != 0) {
        memcpy(frame, real_mac, 6);
    }
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

        /* 5a. Wyczyść tablicę MAC-NAT (nowa sesja bridgingu = nowe mapowania) */
        macnat_clear();
        s_ap_ip_from_sniff = false;

        /* 5b. Przywróć AP do 192.168.4.1 z DHCP (fallback dostępu do GUI).
         *     Po uzyskaniu IP_EVENT_STA_GOT_IP, AP przełączy się
         *     automatycznie na podsieć upstream. */
        ap_restore_management_ip();

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
        s_client_count++;
        ESP_LOGI(TAG, "-> Client joined: " MACSTR " (AID=%d, total=%d)",
                 MAC2STR(ev->mac), ev->aid, s_client_count);

        /* Jeśli jesteśmy w trybie IDLE (brak klona) → klonuj MAC klienta */
        if (s_state == STATE_IDLE && !s_mac_cloned) {
            memcpy(s_client_mac, ev->mac, 6);
            request_mac_clone(ev->mac);
        } else if (s_mac_cloned) {
            /* Bridge aktywny — dodatkowy klient obsługiwany przez MAC-NAT.
             * Jego src MAC jest przepisywany na sklonowany MAC upstream,
             * a odpowiedzi kierowane na podstawie tablicy IP→MAC. */
            ESP_LOGI(TAG, "MAC-NAT: additional client " MACSTR
                     " will use NAT through " MACSTR,
                     MAC2STR(ev->mac), MAC2STR(s_client_mac));
        }
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
        if (s_client_count > 0) s_client_count--;
        ESP_LOGI(TAG, "<- Client left: " MACSTR " (AID=%d, total=%d)",
                 MAC2STR(ev->mac), ev->aid, s_client_count);

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

/* ══════════════════════════════════════════════════════════════
 *  DHCP ACK Sniffer — learn client subnet from bridged DHCP
 *
 *  Podczas bridgingu STA DHCP jest wyłączony (żeby nie kolidował
 *  z klientem). Pakiety DHCP routera przechodzą przez bridge
 *  do telefonu. Sniffujemy DHCP ACK żeby poznać podsieć klienta
 *  i ustawić AP na użyteczny IP w tej samej podsieci.
 * ══════════════════════════════════════════════════════════════ */
static void sniff_dhcp_ack_and_set_ap_ip(const uint8_t *data, uint16_t len)
{
    /* Caller already verified: IPv4, UDP, src:67 dst:68, len >= 286 */

    /* IP header */
    const uint8_t *ip_hdr = data + 14;
    uint8_t ip_ihl = (ip_hdr[0] & 0x0F) * 4;
    const uint8_t *udp_hdr = ip_hdr + ip_ihl;

    /* DHCP message */
    const uint8_t *dhcp = udp_hdr + 8;
    int dhcp_len = len - 14 - ip_ihl - 8;
    if (dhcp_len < 240) return;
    if (dhcp[0] != 2) return;  /* not BOOTREPLY */

    /* Magic cookie 0x63825363 at offset 236 */
    if (dhcp[236] != 0x63 || dhcp[237] != 0x82 ||
        dhcp[238] != 0x53 || dhcp[239] != 0x63) return;

    /* Parse DHCP options — szukamy: type=53 ACK, subnet=1, router=3 */
    const uint8_t *opt = dhcp + 240;
    int opt_max = dhcp_len - 240;
    bool is_ack = false;
    uint32_t subnet_mask = 0;
    uint32_t gateway = 0;

    for (int i = 0; i < opt_max; ) {
        uint8_t type = opt[i];
        if (type == 255) break;           /* End */
        if (type == 0) { i++; continue; } /* Pad */
        if (i + 1 >= opt_max) break;
        uint8_t olen = opt[i + 1];
        if (i + 2 + olen > opt_max) break;

        switch (type) {
        case 53: /* DHCP Message Type */
            if (olen == 1 && opt[i + 2] == 5) is_ack = true;
            break;
        case 1:  /* Subnet Mask */
            if (olen == 4) memcpy(&subnet_mask, &opt[i + 2], 4);
            break;
        case 3:  /* Router */
            if (olen >= 4) memcpy(&gateway, &opt[i + 2], 4);
            break;
        }
        i += 2 + olen;
    }

    if (!is_ack) return;

    /* yiaddr (assigned client IP) at DHCP offset 16 */
    uint32_t client_ip_n;   /* network byte order */
    memcpy(&client_ip_n, &dhcp[16], 4);
    if (client_ip_n == 0 || subnet_mask == 0) return;

    /* Learn IP→MAC from DHCP chaddr (offset 28 in DHCP payload) */
    if (dhcp_len >= 34) {
        macnat_learn(client_ip_n, &dhcp[28]);
    }

    /* AP IP already set from previous DHCP ACK — skip expensive recalculation */
    if (s_ap_ip_from_sniff) return;

    ESP_LOGI(TAG, "DHCP ACK sniffed: client=" IPSTR " mask=" IPSTR " gw=" IPSTR,
             IP2STR((esp_ip4_addr_t *)&client_ip_n),
             IP2STR((esp_ip4_addr_t *)&subnet_mask),
             IP2STR((esp_ip4_addr_t *)&gateway));

    /* Wybierz IP dla AP: najwyższy użyteczny adres w podsieci
     * (broadcast - 1), omijając IP klienta i gateway */
    uint32_t h_client = ntohl(client_ip_n);
    uint32_t h_mask   = ntohl(subnet_mask);
    uint32_t h_gw     = ntohl(gateway);
    uint32_t network  = h_client & h_mask;
    uint32_t bcast    = network | ~h_mask;

    uint32_t candidate = bcast - 1; /* np. x.x.x.254 dla /24 */
    for (int tries = 0; tries < 10; tries++) {
        if (candidate > network && candidate < bcast &&
            candidate != h_client && candidate != h_gw) {
            break;
        }
        candidate--;
    }
    /* Safety: jeśli nie znaleziono, użyj client - 1 */
    if (candidate <= network || candidate >= bcast) {
        candidate = h_client - 1;
        if (candidate <= network) candidate = h_client + 1;
    }

    esp_netif_ip_info_t ap_ip = {
        .ip      = { .addr = htonl(candidate) },
        .netmask = { .addr = subnet_mask },
        .gw      = { .addr = gateway },
    };

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    s_ap_ip_from_sniff = true;

    ESP_LOGI(TAG, "AP IP set to " IPSTR " (reachable from bridged client on same subnet)",
             IP2STR(&ap_ip.ip));
}

/* Przełącz AP na podsieć upstream — zbridgowani klienci
 * widzą GUI pod tym samym IP co ESP STA.
 * Uwaga: ignoruje link-local 169.254.x.x (dummy IP z bridgingu). */
static void ap_mirror_sta_ip(const esp_netif_ip_info_t *sta_ip)
{
    /* Skip link-local (dummy set when STA DHCP off during bridging) */
    uint8_t first_octet = esp_ip4_addr1(&sta_ip->ip);
    uint8_t second_octet = esp_ip4_addr2(&sta_ip->ip);
    if (first_octet == 169 && second_octet == 254) {
        ESP_LOGW(TAG, "Ignoring link-local STA IP " IPSTR " — waiting for DHCP ACK sniff",
                 IP2STR(&sta_ip->ip));
        return;
    }
    /* Skip zeroed IPs */
    if (sta_ip->ip.addr == 0) {
        ESP_LOGW(TAG, "Ignoring zero STA IP");
        return;
    }

    esp_netif_dhcps_stop(s_ap_netif);          /* wyłącz DHCP — upstream DHCP obsługuje klientów */
    esp_netif_ip_info_t ap_ip = {
        .ip      = sta_ip->ip,                 /* ten sam IP co STA */
        .netmask = sta_ip->netmask,
        .gw      = { .addr = 0 },              /* AP nie potrzebuje GW */
    };
    esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    ESP_LOGI(TAG, "AP IP mirrored to " IPSTR " (same subnet as upstream)",
             IP2STR(&ap_ip.ip));
}

/* Przywróć AP do 192.168.4.1 z DHCP (tryb setup/fallback). */
static void ap_restore_management_ip(void)
{
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ap_ip = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
    };
    esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    esp_netif_dhcps_start(s_ap_netif);
    ESP_LOGI(TAG, "AP IP restored to 192.168.4.1 (setup mode, DHCP ON)");
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "=== Got IP: " IPSTR " gw: " IPSTR " ===",
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);

        /* Przełącz AP na podsieć upstream — GUI dostępne pod STA IP */
        ap_mirror_sta_ip(&ev->ip_info);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "STA lost IP, restoring AP management subnet");
        ap_restore_management_ip();
    }
}

/* ══════════════════════════════════════════════════════════════
 *  WiFi info + status
 * ══════════════════════════════════════════════════════════════ */

static void print_wifi_info(void)
{
    ESP_LOGI(TAG, "");
#if SOC_WIFI_HE_SUPPORT
    ESP_LOGI(TAG, "=== WiFi 6 (802.11ax) Repeater ===");
    ESP_LOGI(TAG, "  HE (High Efficiency): CAPABLE");
    ESP_LOGI(TAG, "  OFDMA / BSS Coloring: CAPABLE");
    ESP_LOGI(TAG, "  MCS 0-9:              YES");
    ESP_LOGI(TAG, "  BW: HT20 (required for HE)");
    ESP_LOGI(TAG, "  (WiFi6 active only if upstream AP supports it)");
#else
    ESP_LOGI(TAG, "=== WiFi 5 (802.11n) Repeater ===");
    ESP_LOGI(TAG, "  BW: HT40 (2.4 GHz)");
#endif
    ESP_LOGI(TAG, "  Compat: WiFi 4/5");
    ESP_LOGI(TAG, "  Security: WPA2/WPA3");
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
#else
            ESP_LOGI(TAG, "  PHY: %s",
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

    /* AP DHCP server ON przy starcie (tryb konfiguracji).
     * Zanim STA połączy się z routerem, klient AP dostaje
     * 192.168.4.x i konfiguruje repeater pod http://192.168.4.1
     * Po uzyskaniu IP z upstream (IP_EVENT_STA_GOT_IP),
     * AP IP zmienia się na tę samą podsieć co upstream —
     * dzięki temu zbridgowany klient osiąga GUI bez zmiany IP. */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Zachowaj oryginalny MAC */
    esp_read_mac(s_original_sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(s_ap_mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(s_original_sta_mac));
    ESP_LOGI(TAG, "AP  MAC: " MACSTR, MAC2STR(s_ap_mac));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* STA config — from NVS runtime config */
    wifi_config_t sta_cfg = {
        .sta = {
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
    strlcpy((char *)sta_cfg.sta.ssid,     s_cfg.sta_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password,  s_cfg.sta_pass, sizeof(sta_cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    /* AP config — from NVS runtime config */
    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 0,
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            .pmf_cfg = { .required = false, .capable = true },
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid,     s_cfg.ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, s_cfg.ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len       = strlen(s_cfg.ap_ssid);
    ap_cfg.ap.max_connection = s_cfg.max_clients;
    if (strlen(s_cfg.ap_pass) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Bandwidth: HE (C6) wymaga HT20; bez HE (S3) HT40 daje lepszy throughput */
#if SOC_WIFI_HE_SUPPORT
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT20);
#else
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
    esp_wifi_set_bandwidth(WIFI_IF_AP,  WIFI_BW_HT40);
#endif
    esp_wifi_set_max_tx_power(s_cfg.tx_power_dbm * 4);

    /* Event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL, NULL));
}

/* ══════════════════════════════════════════════════════════════
 *  app_main
 * ══════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
#if SOC_WIFI_HE_SUPPORT
    ESP_LOGI(TAG, "  ESP32 WiFi 6 Repeater (no NAT)");
#else
    ESP_LOGI(TAG, "  ESP32 WiFi Repeater (no NAT)");
#endif
    ESP_LOGI(TAG, "  L2 Bridge - MAC Cloning + MAC-NAT");
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

    /* Load runtime config from NVS (falls back to menuconfig defaults) */
    repeater_config_load(&s_cfg);

    print_wifi_info();
    init_wifi();

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "APSTA started");
    ESP_LOGI(TAG, "  Upstream: %s", s_cfg.sta_ssid);
    ESP_LOGI(TAG, "  Repeater: %s", s_cfg.ap_ssid);
    ESP_LOGI(TAG, "  TX Power: %d dBm, Max clients: %d",
             s_cfg.tx_power_dbm, s_cfg.max_clients);
    ESP_LOGI(TAG, "  Config GUI: http://192.168.4.1 (before upstream connect)");
    ESP_LOGI(TAG, "              After upstream connect: same IP as STA");

    /* Start HTTP config server (if enabled in menuconfig) */
    repeater_httpd_start();

    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Waiting for connections...");
}
