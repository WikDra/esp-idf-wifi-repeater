/*
 * ESP32 WiFi Repeater (bez NAT, ta sama podsieć)
 *
 * Wymaga ESP-IDF v6.1 lub nowszego.
 *
 * Obsługiwane SoC:
 *   ESP32-C5  — WiFi 6 (802.11ax), dual-band 2.4 GHz + 5 GHz
 *   ESP32-C6  — WiFi 6 (802.11ax), 2.4 GHz
 *   ESP32-S3  — WiFi 4 (802.11n),  2.4 GHz
 *   ESP32-C3  — WiFi 4 (802.11n),  2.4 GHz
 *   ESP32     — WiFi 4 (802.11b/g/n), 2.4 GHz
 *
 * Architektura:
 *   SoC działa w trybie APSTA (jednoczesne STA + SoftAP).
 *   STA łączy się z upstream AP (router). AP tworzy sieć dla klientów.
 *   Pakiety są bridgowane na warstwie L2 między interfejsami.
 *
 * Pasma (ESP32-C5):
 *   C5 ma JEDNO radio — WIFI_BAND_MODE_AUTO nie znaczy "dual-band
 *   jednocześnie", tylko "wybierz pasmo automatycznie". Po połączeniu
 *   STA na kanale 5 GHz SoftAP jest przenoszony na ten sam kanał
 *   (kanał STA ma wyższy priorytet niż kanał AP).
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
 * Ograniczenie: MAC cloning obsługuje jednego klienta "primary"
 * (STA może mieć tylko jeden MAC). Pozostali klienci są obsługiwani
 * przez MAC-NAT (przepisywanie adresów L2 + tablica IP→MAC).
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
#include "soc/soc_caps.h"
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

/* Cached IPs for fast hot-path comparison (network byte order).
 * Updated when AP/STA IP changes. Avoids esp_netif_get_ip_info() in hot-path. */
static uint32_t s_ap_ip_cache  = 0;  /* nasz AP IP (np. 192.168.8.254) */
static uint32_t s_sta_ip_cache = 0;  /* nasz STA IP (link-local dummy lub DHCP) */

/**
 * Fast hot-path filter: should this broadcast/multicast frame go to lwIP?
 *
 * The repeater only needs lwIP for:
 *  - ARP requests targeting our own IP (so the HTTP GUI / management responds)
 *
 * Everything else (mDNS, SSDP, NetBIOS, IPv6 multicast, broadcast DHCP for
 * other hosts, etc.) is forwarded at L2 but does NOT need to enter our stack.
 *
 * Returns true  → pass to esp_netif_receive()
 * Returns false → free the buffer, skip lwIP
 */
static inline bool is_broadcast_for_us(const uint8_t *frame, uint16_t len,
                                        uint32_t our_ip1, uint32_t our_ip2)
{
    /* EtherType at offset 12-13 */
    uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];

    /* ARP (0x0806) — pass only REQUEST (opcode 1) targeting our IP */
    if (ethertype == 0x0806 && len >= 42) {
        uint16_t opcode = ((uint16_t)frame[20] << 8) | frame[21];
        if (opcode == 1) {  /* ARP REQUEST */
            uint32_t target_ip;
            memcpy(&target_ip, frame + 38, 4);  /* ARP target protocol address */
            if (target_ip == our_ip1 && our_ip1 != 0) return true;
            if (target_ip == our_ip2 && our_ip2 != 0) return true;
        }
    }
    return false;
}

/* Runtime config loaded from NVS (or menuconfig defaults) */
static repeater_config_t s_cfg;

static TaskHandle_t s_mac_task_handle = NULL;
static SemaphoreHandle_t s_mac_task_mutex;   /* zapobiega równoległym zmianom MAC */

/* Blokada ponownych prób klonowania po nieudanej próbie (esp_timer_get_time). */
#define MAC_CLONE_COOLDOWN_US (60 * 1000000LL)
static int64_t s_clone_block_until = 0;

/* ── Backoff auto-reconnectu STA ──────────────────────────────
 * Każde esp_wifi_connect() przy band mode AUTO to skan wszystkich kanałów
 * w 2.4 i 5 GHz. C5 ma jedno radio, więc w tym czasie SoftAP schodzi z kanału
 * i klient nie dokończy nawet DHCP. Dlatego przy powtarzających się
 * niepowodzeniach wydłużamy odstęp, a gdy ktoś jest podłączony do naszego AP
 * i nie mamy upstreamu (tryb konfiguracji) — trzymamy maksymalny odstęp,
 * żeby GUI było responsywne.
 *
 * Reconnect jest odpalany z esp_timer, NIE z vTaskDelay() w handlerze eventów:
 * blokowanie pętli eventów opóźniało obsługę zdarzeń klientów AP. */
#define RECONNECT_DELAY_MIN_MS   1000
#define RECONNECT_DELAY_MAX_MS  30000
static esp_timer_handle_t s_reconnect_timer = NULL;
static int s_reconnect_fails = 0;

static void reconnect_timer_cb(void *arg)
{
    if (s_suppress_auto_reconnect || s_sta_connected) return;
    ESP_LOGI(TAG, "Auto-reconnecting...");
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .name = "sta_reconnect",
        };
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) return;
    }

    uint32_t delay_ms = RECONNECT_DELAY_MIN_MS << (s_reconnect_fails > 5 ? 5 : s_reconnect_fails);
    if (delay_ms > RECONNECT_DELAY_MAX_MS) delay_ms = RECONNECT_DELAY_MAX_MS;
    /* Klient konfiguruje repeater przez GUI — nie przerywaj mu skanami. */
    if (s_client_count > 0) delay_ms = RECONNECT_DELAY_MAX_MS;

    esp_timer_stop(s_reconnect_timer);
    if (esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000) == ESP_OK) {
        ESP_LOGI(TAG, "Retrying upstream in %u ms (attempt %d)",
                 (unsigned)delay_ms, s_reconnect_fails + 1);
    }
    if (s_reconnect_fails < 100) s_reconnect_fails++;
}

/* Forward declarations */
static void ap_mirror_sta_ip(const esp_netif_ip_info_t *sta_ip);
static void ap_restore_management_ip(void);
static void sniff_dhcp_ack_and_set_ap_ip(const uint8_t *data, uint16_t len);
static void macnat_rewrite_upstream(uint8_t *frame, uint16_t len);
static void ap_clone_upstream_ssid(const uint8_t *ssid, uint8_t ssid_len);
static void roaming_task(void *pv);
static void macnat_rewrite_downstream(uint8_t *frame, uint16_t len);
static void macnat_learn(uint32_t ip_n, const uint8_t *mac);
static void request_mac_clone(const uint8_t *client_mac);
static void radio_apply_phy_config(void);
static bool chan_is_5g(uint8_t ch);
static const char *bw_str(wifi_bandwidth_t bw);
static const char *ap_phy_str(const wifi_ap_record_t *ap);
#if SOC_WIFI_SUPPORT_5G
static const char *band_mode_str(uint8_t bm);
#endif

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

    /* Broadcast/multicast: podaj do lwIP TYLKO jeśli to ARP request o nasz IP.
     * Inne broadcasty (mDNS, SSDP, NetBIOS, IGMP) — tylko forward, skip lwIP.
     * Oszczędność: ~10-20k cykli CPU na każdym pominiętym pakiecie. */
    if (dst[0] & 0x01) {
#if CONFIG_REPEATER_BROADCAST_FILTER
        if (is_broadcast_for_us(dst, len, s_sta_ip_cache, s_ap_ip_cache)) {
            esp_netif_receive(s_sta_netif, buffer, len, eb);
            return ESP_OK;
        }
        esp_wifi_internal_free_rx_buffer(eb);
#else
        esp_netif_receive(s_sta_netif, buffer, len, eb);
#endif
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

    /* Broadcast/multicast — forward upstream + podaj do lwIP TYLKO jeśli dla nas */
    if (dst[0] & 0x01) {
        if (s_sta_connected) {
            esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
        }
#if CONFIG_REPEATER_BROADCAST_FILTER
        if (is_broadcast_for_us(dst, len, s_ap_ip_cache, s_sta_ip_cache)) {
            esp_netif_receive(s_ap_netif, buffer, len, eb);
            return ESP_OK;
        }
        esp_wifi_internal_free_rx_buffer(eb);
#else
        esp_netif_receive(s_ap_netif, buffer, len, eb);
#endif
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
            s_clone_block_until = 0;   /* udało się — zdejmij cooldown */
            /* Forwarding jest uruchamiany w STA_CONNECTED handlerze */
        } else {
            ESP_LOGE(TAG, "  Reconnect timeout! Restoring original MAC...");
            s_clone_block_until = esp_timer_get_time() + MAC_CLONE_COOLDOWN_US;
            ESP_LOGW(TAG, "  MAC clone put on %lld s cooldown",
                     (long long)(MAC_CLONE_COOLDOWN_US / 1000000));
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
        s_ap_ip_cache = 0;  /* clear until next DHCP sniff */

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

        /* Check if client(s) connected during restore — they missed
         * the IDLE check in event handler, so trigger clone now.
         * Tylko gdy mamy upstream: bez niego clone→timeout→restore→clone
         * kręciłoby się w nieskończoność (patrz AP_STACONNECTED). */
        if (s_sta_connected) {
            wifi_sta_list_t pending;
            if (esp_wifi_ap_get_sta_list(&pending) == ESP_OK && pending.num > 0) {
                ESP_LOGI(TAG, "Client(s) already connected during restore, "
                         "auto-cloning for " MACSTR, MAC2STR(pending.sta[0].mac));
                memcpy(s_client_mac, pending.sta[0].mac, 6);
                s_client_count = pending.num;
                /* Release mutex BEFORE requesting clone (new task needs it) */
                xSemaphoreGive(s_mac_task_mutex);
                s_mac_task_handle = NULL;
                free(params);
                request_mac_clone(s_client_mac);
                vTaskDelete(NULL);
                return;  /* unreachable, but clear intent */
            }
        }
    }

    xSemaphoreGive(s_mac_task_mutex);
    s_mac_task_handle = NULL;
    free(params);
    vTaskDelete(NULL);
}

static void request_mac_clone(const uint8_t *client_mac)
{
    /* Cooldown po nieudanej próbie: jeśli router uparcie odrzuca sklonowany
     * MAC, bez tego kręcilibyśmy clone→timeout→restore→clone bez końca,
     * a każdy cykl zrywa klientowi DHCP i dostęp do GUI. */
    if (s_clone_block_until && esp_timer_get_time() < s_clone_block_until) {
        ESP_LOGW(TAG, "MAC clone in cooldown (%lld s left), staying in IDLE",
                 (long long)((s_clone_block_until - esp_timer_get_time()) / 1000000));
        return;
    }
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
        ESP_LOGI(TAG, ">> Connected to: %.*s (ch %d / %s, BSSID " MACSTR ")",
                 ev->ssid_len, ev->ssid, ev->channel,
                 chan_is_5g(ev->channel) ? "5 GHz" : "2.4 GHz", MAC2STR(ev->bssid));
        s_sta_connected = true;
        s_reconnect_fails = 0;   /* połączyliśmy się — zeruj backoff */
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

        /* Klonuj SSID upstream do AP (jeśli włączone) */
        ap_clone_upstream_ssid(ev->ssid, ev->ssid_len);

        /* Jeśli jesteśmy w trybie bridging (MAC cloned), włącz forwarding */
        if (s_mac_cloned) {
            forwarding_start();
            break;
        }

        /* Upstream właśnie się pojawił, a klienci czekają już podłączeni do AP
         * (klonowanie było odłożone w AP_STACONNECTED) → teraz można klonować. */
        if (s_state == STATE_IDLE) {
            wifi_sta_list_t sl;
            if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK && sl.num > 0) {
                ESP_LOGI(TAG, "Upstream up and %d client(s) waiting — cloning MAC for "
                         MACSTR, sl.num, MAC2STR(sl.sta[0].mac));
                s_client_count = sl.num;
                memcpy(s_client_mac, sl.sta[0].mac, 6);
                request_mac_clone(sl.sta[0].mac);
            }
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

        /* Auto-reconnect, ale NIE gdy mac_change_task sam zarządza połączeniem.
         * Odstęp rośnie przy powtarzających się niepowodzeniach — patrz
         * schedule_reconnect(). Nigdy nie blokuj tu pętli eventów. */
        if (!s_suppress_auto_reconnect) {
            schedule_reconnect();
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        /* Use actual sta_list for reliable count (manual tracking desyncs
         * from duplicate leave events caused by SA Query timeouts) */
        {
            wifi_sta_list_t sl;
            s_client_count = (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) ? sl.num : s_client_count + 1;
        }
        ESP_LOGI(TAG, "-> Client joined: " MACSTR " (AID=%d, total=%d)",
                 MAC2STR(ev->mac), ev->aid, s_client_count);

        /* Klonuj MAC tylko gdy STA jest faktycznie połączone z upstreamem.
         * Bez upstreamu klonowanie nie ma sensu i jest wręcz szkodliwe:
         * reconnect ze sklonowanym MAC-iem nie ma z czym się połączyć, więc
         * po 15 s timeout wraca restore, a stąd znowu clone — pętla, w której
         * DHCP na STA i IP AP ciągle się przestawiają, a klient traci panel.
         * Dopóki nie ma upstreamu zostajemy w IDLE i po prostu serwujemy GUI
         * pod 192.168.4.1 (tryb konfiguracji). Gdy STA się w końcu połączy,
         * handler WIFI_EVENT_STA_CONNECTED dokona klonowania. */
        if (!s_sta_connected) {
            ESP_LOGI(TAG, "   No upstream yet — staying in setup mode "
                     "(GUI at http://192.168.4.1), MAC clone deferred");
        } else if (s_state == STATE_IDLE && !s_mac_cloned) {
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
        /* Use actual sta_list for reliable count (exclude leaving client) */
        {
            wifi_sta_list_t sl;
            if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
                int cnt = 0;
                for (int i = 0; i < sl.num; i++) {
                    if (memcmp(sl.sta[i].mac, ev->mac, 6) != 0) cnt++;
                }
                s_client_count = cnt;
            } else if (s_client_count > 0) {
                s_client_count--;
            }
        }
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
    s_ap_ip_cache = ap_ip.ip.addr;  /* update cache for hot-path filter */

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
    s_ap_ip_cache = ap_ip.ip.addr;  /* update cache for hot-path filter */
    ESP_LOGI(TAG, "AP IP mirrored to " IPSTR " (same subnet as upstream)",
             IP2STR(&ap_ip.ip));
}

/* Przywróć AP do 192.168.4.1 z DHCP (tryb setup/fallback).
 *
 * Idempotentne z premedytacją: jeśli AP już ma to IP, wychodzimy bez
 * dotykania serwera DHCP. Restart dhcps zrywa trwające transakcje klientów —
 * telefon, który jest w trakcie "uzyskiwanie adresu IP", po prostu się poddaje.
 * A wołane jest to z IP_EVENT_STA_LOST_IP, czyli przy KAŻDEJ nieudanej próbie
 * połączenia z upstreamem. */
static void ap_restore_management_ip(void)
{
    const uint32_t mgmt_ip = ESP_IP4TOADDR(192, 168, 4, 1);

    esp_netif_ip_info_t cur;
    if (esp_netif_get_ip_info(s_ap_netif, &cur) == ESP_OK && cur.ip.addr == mgmt_ip) {
        s_ap_ip_cache = mgmt_ip;   /* odśwież cache hot-path i nic więcej */
        return;
    }

    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_ip_info_t ap_ip = {
        .ip      = { .addr = mgmt_ip },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = mgmt_ip },
    };
    esp_netif_set_ip_info(s_ap_netif, &ap_ip);
    s_ap_ip_cache = ap_ip.ip.addr;  /* update cache for hot-path filter */
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
        s_sta_ip_cache = ev->ip_info.ip.addr;  /* cache for hot-path filter */
        xEventGroupSetBits(s_wifi_event_group, STA_CONNECTED_BIT);

        /* Przełącz AP na podsieć upstream — GUI dostępne pod STA IP */
        ap_mirror_sta_ip(&ev->ip_info);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "STA lost IP, restoring AP management subnet");
        s_sta_ip_cache = 0;
        ap_restore_management_ip();
    }
}

/* ══════════════════════════════════════════════════════════════
 *  WiFi info + status
 * ══════════════════════════════════════════════════════════════ */

#if CONFIG_REPEATER_CPU_STATS
/**
 * Udział zadania IDLE od poprzedniego wywołania, w procentach.
 *
 * Run-time stats FreeRTOS-a są kumulatywne od bootu, więc liczymy różnicę
 * między raportami — inaczej wynik uśredniałby się do zera po długim uptime.
 * Zwraca -1, gdy nie da się policzyć.
 */
static int cpu_idle_percent(void)
{
    static uint32_t prev_idle = 0, prev_total = 0;

    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *tasks = calloc(count, sizeof(TaskStatus_t));
    if (!tasks) return -1;

    uint32_t total = 0;
    count = uxTaskGetSystemState(tasks, count, &total);

    uint32_t idle = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        /* Nazwy zadań bezczynności to "IDLE" (unicore) albo "IDLE0"/"IDLE1". */
        if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) {
            idle += tasks[i].ulRunTimeCounter;
        }
    }
    free(tasks);

    int result = -1;
    if (prev_total && total > prev_total) {
        uint32_t d_total = total - prev_total;
        uint32_t d_idle  = idle >= prev_idle ? idle - prev_idle : 0;
        if (d_idle > d_total) d_idle = d_total;
        result = (int)((d_idle * 100) / d_total);
    }
    prev_idle = idle;
    prev_total = total;
    return result;
}
#endif /* CONFIG_REPEATER_CPU_STATS */

static void print_wifi_info(void)
{
    ESP_LOGI(TAG, "");
#if SOC_WIFI_SUPPORT_5G
    ESP_LOGI(TAG, "=== WiFi 6 dual-band (2.4 + 5 GHz) Repeater ===");
    ESP_LOGI(TAG, "  HE (High Efficiency): CAPABLE (2.4 & 5 GHz)");
    ESP_LOGI(TAG, "  5 GHz PHY:            11a / 11n / 11ac / 11ax");
    ESP_LOGI(TAG, "  Band mode:            %s", band_mode_str(s_cfg.band_mode));
    ESP_LOGI(TAG, "  BW request:           2.4G=%s  5G=%s",
             bw_str((wifi_bandwidth_t)s_cfg.bw_2g), bw_str((wifi_bandwidth_t)s_cfg.bw_5g));
    ESP_LOGI(TAG, "  5 GHz preference:     +%d dB over 2.4 GHz", s_cfg.rssi_5g_adj);
    ESP_LOGI(TAG, "  NOTE: single radio — 'auto' selects a band, not both at once");
#elif SOC_WIFI_HE_SUPPORT
    ESP_LOGI(TAG, "=== WiFi 6 (802.11ax) Repeater — 2.4 GHz ===");
    ESP_LOGI(TAG, "  HE (High Efficiency): CAPABLE");
    ESP_LOGI(TAG, "  OFDMA / BSS Coloring: CAPABLE");
    ESP_LOGI(TAG, "  BW request:           %s", bw_str((wifi_bandwidth_t)s_cfg.bw_2g));
    ESP_LOGI(TAG, "  (WiFi6 active only if upstream AP supports it)");
#else
    ESP_LOGI(TAG, "=== WiFi 4 (802.11n) Repeater — 2.4 GHz ===");
    ESP_LOGI(TAG, "  BW request:           %s", bw_str((wifi_bandwidth_t)s_cfg.bw_2g));
#endif
    ESP_LOGI(TAG, "  Compat: WiFi 4/5/6");
    ESP_LOGI(TAG, "  Security: WPA2/WPA3");
    ESP_LOGI(TAG, "===================================");
}

static void status_task(void *pv)
{
    /* Pierwszy raport szybko — po podłączeniu monitora do USB-Serial/JTAG
     * logi bootu są już nie do odzyskania, a to jest jedyne miejsce, gdzie
     * widać realny stan radia. Potem normalnie co 30 s. */
    TickType_t delay = pdMS_TO_TICKS(8000);

    while (1) {
        vTaskDelay(delay);
        delay = pdMS_TO_TICKS(30000);

        const char *state_str;
        switch (s_state) {
            case STATE_IDLE:          state_str = "IDLE"; break;
            case STATE_MAC_CHANGING:  state_str = "MAC_CHANGING"; break;
            case STATE_BRIDGING:      state_str = "BRIDGING"; break;
            case STATE_MAC_RESTORING: state_str = "MAC_RESTORING"; break;
            default:                  state_str = "UNKNOWN"; break;
        }

        ESP_LOGI(TAG, "--- Status [%s] ---", state_str);

        /* Realny stan radia — niezależny od tego, czy STA jest połączone. */
#if SOC_WIFI_SUPPORT_5G
        {
            wifi_band_mode_t bm;
            wifi_bandwidths_t bws;
            if (esp_wifi_get_band_mode(&bm) == ESP_OK) {
                ESP_LOGI(TAG, "  Band mode: %s", band_mode_str((uint8_t)bm));
            }
            if (esp_wifi_get_bandwidths(WIFI_IF_STA, &bws) == ESP_OK) {
                ESP_LOGI(TAG, "  BW cfg: 2.4 GHz=%s, 5 GHz=%s",
                         bw_str(bws.ghz_2g), bw_str(bws.ghz_5g));
            }
        }
#endif
        {
            wifi_protocols_t pr;
            if (esp_wifi_get_protocols(WIFI_IF_STA, &pr) == ESP_OK) {
                ESP_LOGI(TAG, "  Protocols: 2.4G=0x%02x 5G=0x%02x", pr.ghz_2g, pr.ghz_5g);
            }
        }

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "  Up: %s RSSI:%d Ch:%d (%s)",
                     ap.ssid, ap.rssi, ap.primary,
                     chan_is_5g(ap.primary) ? "5 GHz" : "2.4 GHz");
            ESP_LOGI(TAG, "  Link PHY: %s @ %s", ap_phy_str(&ap), bw_str(ap.bandwidth));
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
#if CONFIG_REPEATER_CPU_STATS
        {
            int idle = cpu_idle_percent();
            if (idle >= 0) {
                ESP_LOGI(TAG, "  CPU: %d%% busy, %d%% idle  (idle near 0 = CPU bound, "
                         "otherwise airtime bound)", 100 - idle, idle);
            }
        }
#endif
        ESP_LOGI(TAG, "---");
    }
}

/* ══════════════════════════════════════════════════════════════
 *  AP SSID Cloning — kopiuje SSID upstream AP na Repeater AP
 * ══════════════════════════════════════════════════════════════ */

static void ap_clone_upstream_ssid(const uint8_t *ssid, uint8_t ssid_len)
{
    if (!s_cfg.ap_clone_ssid) return;

    wifi_config_t ap_cfg;
    esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);

    /* Sprawdź czy SSID się zmieniło */
    if (ap_cfg.ap.ssid_len == ssid_len &&
        memcmp(ap_cfg.ap.ssid, ssid, ssid_len) == 0) {
        return;  /* already cloned */
    }

    memset(ap_cfg.ap.ssid, 0, sizeof(ap_cfg.ap.ssid));
    memcpy(ap_cfg.ap.ssid, ssid, ssid_len);
    ap_cfg.ap.ssid_len = ssid_len;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    ESP_LOGW(TAG, "AP SSID cloned to: %.*s", ssid_len, ssid);
}

/* ══════════════════════════════════════════════════════════════
 *  Pseudo-mesh roaming — scan for better AP with same SSID
 *
 *  Monitoruje RSSI upstream AP. Jeśli spadnie poniżej progu,
 *  skanuje w poszukiwaniu innego AP z tym samym SSID ale
 *  innym BSSID (pomijając własny AP). Przełącza się na najlepszy.
 * ══════════════════════════════════════════════════════════════ */

static void roaming_task(void *pv)
{
    ESP_LOGI(TAG, "Pseudo-mesh roaming started (threshold=%d dBm, hysteresis=%d dB)",
             (int)s_cfg.roam_rssi_threshold, (int)s_cfg.roam_hysteresis);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));  /* sprawdzaj co 10s */

        /* Roaming tylko gdy STA jest podłączony i nie trwa zmiana MAC */
        if (!s_sta_connected || s_state == STATE_MAC_CHANGING ||
            s_state == STATE_MAC_RESTORING || s_suppress_auto_reconnect) {
            continue;
        }

        wifi_ap_record_t current_ap;
        if (esp_wifi_sta_get_ap_info(&current_ap) != ESP_OK) {
            continue;
        }

        /* RSSI powyżej progu — nie szukaj lepszego */
        if (current_ap.rssi >= s_cfg.roam_rssi_threshold) {
            continue;
        }

        ESP_LOGW(TAG, "ROAM: RSSI=%d < threshold=%d, scanning for better AP...",
                 current_ap.rssi, (int)s_cfg.roam_rssi_threshold);

        /* Scan pasywny (nie rozłącza STA) */
        wifi_scan_config_t scan_cfg = {
            .ssid = current_ap.ssid,   /* szukaj tego samego SSID */
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 100,
            .scan_time.active.max = 300,
        };

        /* esp_wifi_scan_start z block=true może trwać kilka sekund */
        esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ROAM: scan failed: %s", esp_err_to_name(err));
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count == 0) {
            esp_wifi_scan_get_ap_records(&ap_count, NULL);  /* free scan memory */
            ESP_LOGI(TAG, "ROAM: no APs found with SSID '%s'", current_ap.ssid);
            continue;
        }

        wifi_ap_record_t *ap_list = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (!ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, NULL);
            continue;
        }
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);

        /* Znajdź najlepszy AP (pomijając siebie i obecny).
         * Ocena = RSSI + bonus za 5 GHz, tak samo jak polityka wyboru AP
         * przy zwykłym łączeniu (threshold.rssi_5g_adjustment). */
        int best_idx = -1;
        int best_score = -127;

        for (int i = 0; i < ap_count; i++) {
            /* Pomiń własny AP (BSSID = s_ap_mac) */
            if (memcmp(ap_list[i].bssid, s_ap_mac, 6) == 0) {
                ESP_LOGI(TAG, "ROAM: skip self " MACSTR, MAC2STR(ap_list[i].bssid));
                continue;
            }
            /* Pomiń obecny AP */
            if (memcmp(ap_list[i].bssid, s_upstream_bssid, 6) == 0) {
                continue;
            }
            int score = ap_list[i].rssi;
#if SOC_WIFI_SUPPORT_5G
            if (chan_is_5g(ap_list[i].primary)) score += s_cfg.rssi_5g_adj;
#endif
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }

        if (best_idx < 0) {
            ESP_LOGI(TAG, "ROAM: no better AP found");
            free(ap_list);
            continue;
        }

        int best_rssi = ap_list[best_idx].rssi;

        /* Nowy AP musi być lepszy o hysteresis od obecnego (po uwzględnieniu
         * bonusu za 5 GHz), inaczej ciągle byśmy się przełączali. */
        int current_score = current_ap.rssi;
#if SOC_WIFI_SUPPORT_5G
        if (chan_is_5g(current_ap.primary)) current_score += s_cfg.rssi_5g_adj;
#endif
        if (best_score < current_score + s_cfg.roam_hysteresis) {
            ESP_LOGI(TAG, "ROAM: best candidate " MACSTR " RSSI=%d ch=%d (%s), "
                     "not enough improvement (need +%d dB over score %d)",
                     MAC2STR(ap_list[best_idx].bssid), best_rssi,
                     ap_list[best_idx].primary,
                     chan_is_5g(ap_list[best_idx].primary) ? "5 GHz" : "2.4 GHz",
                     (int)s_cfg.roam_hysteresis, current_score);
            free(ap_list);
            continue;
        }

        /* ── Roam! ──────────────────────────── */
        ESP_LOGW(TAG, "ROAM: switching to " MACSTR " RSSI=%d ch=%d (%s) "
                 "(from " MACSTR " RSSI=%d ch=%d)",
                 MAC2STR(ap_list[best_idx].bssid), best_rssi,
                 ap_list[best_idx].primary,
                 chan_is_5g(ap_list[best_idx].primary) ? "5 GHz" : "2.4 GHz",
                 MAC2STR(s_upstream_bssid), current_ap.rssi, current_ap.primary);

        /* Zaktualizuj BSSID i kanał */
        memcpy(s_upstream_bssid, ap_list[best_idx].bssid, 6);
        s_upstream_channel = ap_list[best_idx].primary;

        /* Ustaw STA config z nowym BSSID */
        wifi_config_t sta_cfg;
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
        memcpy(sta_cfg.sta.bssid, s_upstream_bssid, 6);
        sta_cfg.sta.bssid_set = true;
        sta_cfg.sta.channel = s_upstream_channel;
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

        free(ap_list);

        /* Rozłącz i połącz z nowym AP */
        s_suppress_auto_reconnect = true;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        s_suppress_auto_reconnect = false;
        esp_wifi_connect();

        /* Czekaj na połączenie */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, STA_CONNECTED_BIT,
                                                pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & STA_CONNECTED_BIT) {
            ESP_LOGW(TAG, "ROAM: successfully roamed to " MACSTR, MAC2STR(s_upstream_bssid));
        } else {
            ESP_LOGE(TAG, "ROAM: failed to connect, unlocking BSSID for auto-reconnect");
            /* Odblokuj BSSID */
            esp_wifi_get_config(WIFI_IF_STA, &sta_cfg);
            sta_cfg.sta.bssid_set = false;
            sta_cfg.sta.channel = 0;
            esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
            s_bssid_locked = false;
            esp_wifi_connect();
        }

        /* Daj trochę czasu na stabilizację przed kolejnym sprawdzeniem */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Radio / PHY helpers (band, protokoły, bandwidth)
 * ══════════════════════════════════════════════════════════════ */

/* Kanały 2.4 GHz to 1–14, kanały 5 GHz zaczynają się od 36. */
static bool chan_is_5g(uint8_t ch) { return ch >= 36; }

static const char *bw_str(wifi_bandwidth_t bw)
{
    switch (bw) {
    case WIFI_BW20:      return "20 MHz";
    case WIFI_BW40:      return "40 MHz";
    case WIFI_BW80:      return "80 MHz";
    case WIFI_BW160:     return "160 MHz";
    case WIFI_BW80_BW80: return "80+80 MHz";
    default:             return "?";
    }
}

/* Najwyższy standard, na którym pracuje dany AP (do logów / GUI). */
static const char *ap_phy_str(const wifi_ap_record_t *ap)
{
    if (ap->phy_11ax) return "WiFi6 (11ax)";
    if (ap->phy_11ac) return "WiFi5 (11ac)";
    if (ap->phy_11n)  return "WiFi4 (11n)";
    if (ap->phy_11a)  return "11a";
    if (ap->phy_11g)  return "11g";
    if (ap->phy_11b)  return "11b";
    return "Legacy";
}

#if SOC_WIFI_SUPPORT_5G
static const char *band_mode_str(uint8_t bm)
{
    switch (bm) {
    case WIFI_BAND_MODE_2G_ONLY: return "2.4 GHz only";
    case WIFI_BAND_MODE_5G_ONLY: return "5 GHz only";
    case WIFI_BAND_MODE_AUTO:    return "2.4 GHz + 5 GHz (auto)";
    default:                     return "?";
    }
}
#endif

/**
 * Ustaw pasmo, protokoły i szerokość kanału — TYLKO dla interfejsu STA.
 *
 * Wołane gdy tryb radia to jeszcze WIFI_MODE_STA, tuż po esp_wifi_start()
 * i przed uruchomieniem SoftAP. To nie jest kaprys:
 *
 *  - `esp_wifi_set_band_mode()` wymaga wystartowanego WiFi
 *    (`ESP_ERR_WIFI_NOT_STARTED`), więc nie da się tego zrobić w init.
 *  - Zmiana pasma / protokołu / bandwidth na DZIAŁAJĄCYM SoftAP (tryb APSTA,
 *    AP już bikonuje) zawiesza CPU tak, że pomaga tylko BOOT+RESET.
 *    Dlatego AP jest dodawany dopiero po skonfigurowaniu PHY, a jego
 *    parametry PHY zostawiamy sterownikowi — w APSTA kanał i pasmo STA
 *    mają wyższy priorytet i AP i tak jest do nich dociągany.
 *
 * Kolejność w środku też jest wymuszona:
 *  1. band mode — kolejne API ignorują pasmo wyłączone przez band mode.
 *  2. protokoły — PRZED bandwidth. 40 MHz i HE/VHT wykluczają się:
 *     sterownik przyjmuje WIFI_BW40 tylko gdy w masce protokołów tego pasma
 *     NIE ma 11AX ani 11AC (patrz $IDF_PATH/examples/wifi/ftm).
 *  3. bandwidth — już spójny z maską protokołów.
 *
 * Wniosek praktyczny dla WiFi 6: HE20 (20 MHz + 11ax) daje 143 Mbps PHY
 * przy 1 strumieniu, a 40 MHz wymaga zejścia do 11n (150 Mbps PHY).
 * Ponieważ realny sufit C5 to ~130 Mbps raw, 40 MHz nic nie zyskuje,
 * a traci OFDMA/MU i odporność HE — dlatego domyślnie jest 20 MHz.
 */
static void radio_apply_phy_config(void)
{
#if SOC_WIFI_SUPPORT_5G
    ESP_LOGI(TAG, "PHY: setting band mode %s...", band_mode_str(s_cfg.band_mode));
    esp_err_t err = esp_wifi_set_band_mode((wifi_band_mode_t)s_cfg.band_mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_band_mode(%d) failed: %s — falling back to 2.4 GHz",
                 s_cfg.band_mode, esp_err_to_name(err));
        s_cfg.band_mode = WIFI_BAND_MODE_2G_ONLY;
        esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    }
#endif

    /* ── Protokoły ──
     * esp_wifi_set_protocols() ustawia MAKSIMUM: 11ax automatycznie
     * dociąga b/g/n (2.4 GHz) oraz a/n/ac (5 GHz).
     * HE/VHT są dodawane TYLKO gdy dane pasmo ma 20 MHz. */
    wifi_protocols_t protos = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
        .ghz_5g = 0,
    };
#if SOC_WIFI_HE_SUPPORT
    if (s_cfg.bw_2g == WIFI_BW20) {
        protos.ghz_2g |= WIFI_PROTOCOL_11AX;
    } else {
        ESP_LOGW(TAG, "PHY: 2.4 GHz 40 MHz selected — HE (11ax) disabled on 2.4 GHz");
    }
#endif
#if SOC_WIFI_SUPPORT_5G
    protos.ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N;
    if (s_cfg.bw_5g == WIFI_BW20) {
        protos.ghz_5g |= WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX;
    } else {
        ESP_LOGW(TAG, "PHY: 5 GHz 40 MHz selected — HE/VHT (11ax/11ac) disabled on 5 GHz");
    }
#endif
    ESP_LOGI(TAG, "PHY: protocols 2.4G=0x%02x 5G=0x%02x", protos.ghz_2g, protos.ghz_5g);
    esp_err_t perr = esp_wifi_set_protocols(WIFI_IF_STA, &protos);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_protocols(STA) failed: %s", esp_err_to_name(perr));
    }

    /* ── Bandwidth (spójny z maską protokołów ustawioną powyżej) ── */
#if SOC_WIFI_SUPPORT_5G
    wifi_bandwidths_t bws = {
        .ghz_2g = (wifi_bandwidth_t)s_cfg.bw_2g,
        .ghz_5g = (wifi_bandwidth_t)s_cfg.bw_5g,
    };
    ESP_LOGI(TAG, "PHY: requesting bandwidth 2.4G=%s 5G=%s",
             bw_str(bws.ghz_2g), bw_str(bws.ghz_5g));
    esp_err_t berr = esp_wifi_set_bandwidths(WIFI_IF_STA, &bws);
    if (berr != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_bandwidths(STA) failed: %s", esp_err_to_name(berr));
    }
    {
        wifi_bandwidths_t got = { 0 };
        if (esp_wifi_get_bandwidths(WIFI_IF_STA, &got) == ESP_OK) {
            ESP_LOGI(TAG, "PHY: bandwidth in use 2.4 GHz=%s, 5 GHz=%s",
                     bw_str(got.ghz_2g), bw_str(got.ghz_5g));
        }
    }
#else
    wifi_bandwidth_t bw = (wifi_bandwidth_t)s_cfg.bw_2g;
    esp_wifi_set_bandwidth(WIFI_IF_STA, bw);
    ESP_LOGI(TAG, "PHY: bandwidth 2.4 GHz=%s", bw_str(bw));
#endif

    /* TX power: API przyjmuje ćwiartki dBm */
    esp_wifi_set_max_tx_power(s_cfg.tx_power_dbm * 4);
    ESP_LOGI(TAG, "PHY: config applied");
}

/* ══════════════════════════════════════════════════════════════
 *  Inicjalizacja WiFi — dwie fazy
 *
 *  Faza 1 (init_wifi): netify, esp_wifi_init, tryb STA, config STA.
 *  Faza 2 (start_softap): dopiero PO skonfigurowaniu PHY dokładamy AP.
 *
 *  Rozdzielenie jest konieczne, bo konfiguracja pasma/protokołu/bandwidth
 *  na działającym SoftAP zawiesza CPU (patrz radio_apply_phy_config).
 * ══════════════════════════════════════════════════════════════ */

static void init_wifi(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    assert(s_sta_netif && s_ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Zachowaj oryginalny MAC */
    esp_read_mac(s_original_sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(s_ap_mac, ESP_MAC_WIFI_SOFTAP);
    ESP_LOGI(TAG, "STA MAC: " MACSTR, MAC2STR(s_original_sta_mac));
    ESP_LOGI(TAG, "AP  MAC: " MACSTR, MAC2STR(s_ap_mac));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Start w STA-only — AP dołączy po konfiguracji PHY. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    /* STA config — from NVS runtime config */
    wifi_config_t sta_cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_OPEN,
            /* Preferuj AP 5 GHz o tym samym SSID, dopóki jego RSSI nie jest
             * gorszy od 2.4 GHz o więcej niż rssi_5g_adj dB. Pole jest
             * ignorowane na SoC bez 5 GHz. */
            .threshold.rssi_5g_adjustment = s_cfg.rssi_5g_adj,
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

    /* Event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler, NULL, NULL));
}

/**
 * Dołóż SoftAP do już działającego STA (STA → APSTA).
 *
 * AP DHCP server jest ON przy starcie (tryb konfiguracji): zanim STA połączy
 * się z routerem, klient AP dostaje 192.168.4.x i konfiguruje repeater pod
 * http://192.168.4.1. Po uzyskaniu IP z upstream (IP_EVENT_STA_GOT_IP) AP IP
 * zmienia się na tę samą podsieć co upstream — dzięki temu zbridgowany klient
 * osiąga GUI bez zmiany ustawień IP.
 *
 * Parametrów PHY AP nie ruszamy: w APSTA kanał i pasmo STA mają wyższy
 * priorytet, więc AP zostanie dociągnięty do upstreamu po połączeniu.
 */
static void start_softap(void)
{
    /* Kanał startowy musi leżeć w pasmie wybranym przez band mode. */
    uint8_t ap_channel = 1;
#if SOC_WIFI_SUPPORT_5G
    if (s_cfg.band_mode == WIFI_BAND_MODE_5G_ONLY) {
        ap_channel = 36;   /* najniższy kanał UNII-1, bez DFS */
    }
#endif
    /* PMF (802.11w) tylko tam, gdzie jest obowiązkowe, czyli dla WPA3.
     *
     * Przy WPA2 z PMF "optional" sterownik przy każdej ponownej asocjacji
     * wysyła SA Query i czeka 1 s (assoc_sa_query_max_timeout w
     * wpa_supplicant/src/ap/ap_config.c). Telefony, które nie odpowiadają,
     * są wyrzucane z `reason = 209` (SA_QUERY_TIMEOUT) i wchodzą od nowa —
     * zaobserwowane na Pixelu 7 jako pętla rozłączeń zrywająca DHCP i sesję
     * HTTP w GUI.
     *
     * W esp_hostap.c PMF na AP włącza dopiero pmf_cfg.capable, więc
     * capable=false daje NO_MGMT_FRAME_PROTECTION i problem znika.
     * (Adnotacja "deprecated" przy tym polu dotyczy strony STA, nie AP.) */
    const bool wpa3_authmode = (s_cfg.ap_authmode == WIFI_AUTH_WPA3_PSK) ||
                               (s_cfg.ap_authmode == WIFI_AUTH_WPA2_WPA3_PSK);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = ap_channel,
            .authmode = (wifi_auth_mode_t)s_cfg.ap_authmode,
            .pmf_cfg = { .required = false, .capable = wpa3_authmode },
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
    ESP_LOGI(TAG, "SoftAP: authmode %d, start channel %d, PMF %s",
             ap_cfg.ap.authmode, ap_channel, wpa3_authmode ? "on (WPA3)" : "off (WPA2)");

    /* Kolejność: NAJPIERW włącz interfejs AP (set_mode), POTEM go skonfiguruj.
     * esp_wifi_set_config(WIFI_IF_AP) na wyłączonym interfejsie zwraca
     * ESP_ERR_WIFI_IF — pod ESP_ERROR_CHECK dawało to abort() → panic →
     * reboot → pętla restartów, która objawia się migającym portem USB.
     * Runtime zmiana configu na działającym AP jest bezpieczna (tak samo
     * robi ap_clone_upstream_ssid()). */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %s — staying STA-only",
                 esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s — SoftAP keeps defaults",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SoftAP started (APSTA mode)");
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

    /* Od tego momentu NVS działa — znacz kolejne etapy startu, żeby po
     * ewentualnym zawieszeniu dało się odczytać z hosta, gdzie stanęło
     * (tools/read_boot_stage.py). Patrz repeater_config.h po legendę. */
    repeater_boot_stage_set(2);

    /* Load runtime config from NVS (falls back to menuconfig defaults) */
    repeater_config_load(&s_cfg);

    /* PHY guard: jeśli poprzedni boot nie doszedł do końca konfiguracji radia,
     * wstajemy na bezpiecznych ustawieniach zamiast powtarzać to, co zawiesiło
     * płytkę. Wystarczy potem zmienić ustawienia w GUI. */
    bool phy_safe_mode = repeater_phy_guard_tripped();
    if (phy_safe_mode) {
        ESP_LOGE(TAG, "!!! Previous boot hung while configuring the radio !!!");
        ESP_LOGE(TAG, "    Starting in PHY SAFE MODE: 2.4 GHz, 20 MHz, no band-mode change.");
        ESP_LOGE(TAG, "    Change Band & PHY settings in the web GUI, then reboot.");
        s_cfg.band_mode = 1;   /* WIFI_BAND_MODE_2G_ONLY */
        s_cfg.bw_2g     = 1;   /* WIFI_BW20 */
        s_cfg.bw_5g     = 1;
    }

    print_wifi_info();
    init_wifi();
    repeater_boot_stage_set(3);

    /* Zablokuj auto-connect z WIFI_EVENT_STA_START — najpierw pasmo/protokoły/
     * bandwidth (te API wymagają wystartowanego WiFi), potem SoftAP, i dopiero
     * na końcu łączymy się z upstream AP. */
    s_suppress_auto_reconnect = true;
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA started");
    repeater_boot_stage_set(4);

    if (!phy_safe_mode) {
        repeater_phy_guard_arm();
        radio_apply_phy_config();
        repeater_phy_guard_disarm();
    } else {
        repeater_phy_guard_disarm();
        ESP_LOGW(TAG, "PHY: skipped (safe mode)");
    }
    repeater_boot_stage_set(5);

    start_softap();
    repeater_boot_stage_set(6);

    s_suppress_auto_reconnect = false;
    ESP_LOGI(TAG, "Connecting to upstream...");
    esp_wifi_connect();
    repeater_boot_stage_set(7);

    ESP_LOGI(TAG, "  Upstream: %s", s_cfg.sta_ssid);
    ESP_LOGI(TAG, "  Repeater: %s", s_cfg.ap_ssid);
    ESP_LOGI(TAG, "  TX Power: %d dBm, Max clients: %d",
             s_cfg.tx_power_dbm, s_cfg.max_clients);
#if SOC_WIFI_SUPPORT_5G
    ESP_LOGI(TAG, "  Band mode: %s", band_mode_str(s_cfg.band_mode));
#endif
    ESP_LOGI(TAG, "  AP Clone SSID: %s", s_cfg.ap_clone_ssid ? "ON" : "OFF");
    ESP_LOGI(TAG, "  Pseudo-mesh: %s", s_cfg.pseudo_mesh ? "ON" : "OFF");
    if (s_cfg.pseudo_mesh) {
        ESP_LOGI(TAG, "    RSSI threshold: %d dBm, Hysteresis: %d dB",
                 (int)s_cfg.roam_rssi_threshold, (int)s_cfg.roam_hysteresis);
    }
    ESP_LOGI(TAG, "  Config GUI: http://192.168.4.1 (before upstream connect)");
    ESP_LOGI(TAG, "              After upstream connect: same IP as STA");

    /* Start HTTP config server (if enabled in menuconfig) */
    repeater_httpd_start();
    repeater_boot_stage_set(8);

    xTaskCreate(status_task, "status", 4096, NULL, 5, NULL);

    /* Start roaming task if pseudo-mesh enabled */
    if (s_cfg.pseudo_mesh) {
        xTaskCreate(roaming_task, "roaming", 4096, NULL, 5, NULL);
    }

    repeater_boot_stage_set(9);
    ESP_LOGI(TAG, "Waiting for connections...");
}
