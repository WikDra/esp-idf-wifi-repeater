/*
 * repeater_httpd.c — HTTP configuration server
 *
 * Minimalist web GUI for the WiFi 6 repeater.
 * GET  /        → config page (HTML form)
 * POST /save    → save config to NVS + reboot
 * POST /reset   → reset config to Kconfig defaults + reboot
 * GET  /status  → JSON status (AJAX-friendly)
 */

#include "sdkconfig.h"

#if CONFIG_REPEATER_HTTPD_ENABLE

#include <string.h>
#include <stdlib.h>
#include "repeater_httpd.h"
#include "repeater_config.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char *TAG = "rep_httpd";
static httpd_handle_t s_server = NULL;

/* ── HTML ────────────────────────────────────────────────────── */

static const char HTML_PAGE[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>WiFi6 Repeater</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:1rem}"
".c{max-width:480px;margin:0 auto}"
"h1{text-align:center;font-size:1.4rem;margin-bottom:.5rem;color:#38bdf8}"
".sub{text-align:center;color:#64748b;font-size:.85rem;margin-bottom:1.5rem}"
".card{background:#1e293b;border-radius:12px;padding:1.2rem;margin-bottom:1rem;border:1px solid #334155}"
".card h2{font-size:1rem;color:#38bdf8;margin-bottom:.8rem;padding-bottom:.5rem;border-bottom:1px solid #334155}"
"label{display:block;font-size:.85rem;color:#94a3b8;margin-bottom:.25rem;margin-top:.6rem}"
"label:first-child{margin-top:0}"
"input[type=text],input[type=password],input[type=number]{"
"width:100%%;padding:.55rem .7rem;border:1px solid #475569;border-radius:8px;"
"background:#0f172a;color:#e2e8f0;font-size:.95rem;outline:none;transition:border .2s}"
"input:focus{border-color:#38bdf8}"
".row{display:flex;gap:.6rem}"
".row>div{flex:1}"
".btn{display:block;width:100%%;padding:.7rem;border:none;border-radius:8px;"
"font-size:1rem;font-weight:600;cursor:pointer;transition:background .2s;margin-top:.5rem}"
".btn-save{background:#2563eb;color:#fff}.btn-save:hover{background:#1d4ed8}"
".btn-rst{background:#334155;color:#94a3b8;font-size:.85rem;margin-top:.4rem}"
".btn-rst:hover{background:#475569;color:#e2e8f0}"
".st{font-size:.82rem;color:#94a3b8;line-height:1.6}"
".st b{color:#e2e8f0;font-weight:500}"
".g{color:#4ade80}.r{color:#f87171}"
"#msg{text-align:center;padding:.6rem;border-radius:8px;margin-bottom:.8rem;display:none;"
"background:#164e63;color:#22d3ee;font-size:.9rem}"
"</style></head><body>"
"<div class='c'>"
"<h1>&#128225; WiFi6 Repeater</h1>"
"<p class='sub'>ESP32-C6 &middot; L2 Bridge &middot; No NAT</p>"
"<div id='msg'></div>"
/* Status card (filled by JS) */
"<div class='card' id='scard'>"
"<h2>&#128504; Status</h2>"
"<div class='st' id='status'>Loading...</div>"
"</div>"
/* Config form */
"<form method='POST' action='/save'>"
"<div class='card'>"
"<h2>&#128225; Upstream AP (STA)</h2>"
"<label>SSID</label>"
"<input name='sta_ssid' type='text' maxlength='32' value='%s' required>"
"<label>Password</label>"
"<input name='sta_pass' type='password' maxlength='64' value='%s'>"
"</div>"
"<div class='card'>"
"<h2>&#128246; Repeater AP</h2>"
"<label>SSID</label>"
"<input name='ap_ssid' type='text' maxlength='32' value='%s' required>"
"<label>Password</label>"
"<input name='ap_pass' type='password' maxlength='64' value='%s'>"
"<div class='row'><div>"
"<label>Max clients</label>"
"<input name='max_cli' type='number' min='1' max='10' value='%d'>"
"</div><div>"
"<label>TX Power (dBm)</label>"
"<input name='tx_pwr' type='number' min='2' max='20' value='%d'>"
"</div></div>"
"</div>"
"<button class='btn btn-save' type='submit'>&#128190; Save &amp; Reboot</button>"
"</form>"
"<form method='POST' action='/reset'>"
"<button class='btn btn-rst' type='submit'>&#8635; Reset to defaults</button>"
"</form>"
"</div>"
/* JS: fetch status */
"<script>"
"function fs(){"
"fetch('/status').then(r=>r.json()).then(d=>{"
"let h='';"
"h+='State: <b>'+d.state+'</b><br>';"
"if(d.upstream)h+='Upstream: <b>'+d.upstream+'</b> RSSI:<b>'+d.rssi+'</b> Ch:<b>'+d.channel+'</b><br>';"
"else h+='Upstream: <span class=\"r\">not connected</span><br>';"
"h+='STA MAC: <b>'+d.sta_mac+'</b> '+(d.cloned?'<span class=\"r\">(CLONED)</span>':'')+'<br>';"
"h+='Clients: <b>'+d.clients+'</b><br>';"
"h+='Forwarding: '+(d.forwarding?'<span class=\"g\">ON</span>':'OFF')+'<br>';"
"h+='IP: <b>'+d.ip+'</b><br>';"
"h+='Uptime: <b>'+d.uptime+'</b>s';"
"document.getElementById('status').innerHTML=h;"
"}).catch(()=>{document.getElementById('status').innerHTML='<span class=\"r\">Error</span>'})}"
"fs();setInterval(fs,5000);"
"if(location.search.includes('saved')){"
"let m=document.getElementById('msg');m.textContent='Config saved! Rebooting...';m.style.display='block'}"
"</script>"
"</body></html>";

/* ── URL-decode ──────────────────────────────────────────────── */

static int url_decode_char(const char *s)
{
    int hi = 0, lo = 0;
    if (s[0] >= '0' && s[0] <= '9') hi = s[0] - '0';
    else if (s[0] >= 'A' && s[0] <= 'F') hi = s[0] - 'A' + 10;
    else if (s[0] >= 'a' && s[0] <= 'f') hi = s[0] - 'a' + 10;
    else return -1;
    if (s[1] >= '0' && s[1] <= '9') lo = s[1] - '0';
    else if (s[1] >= 'A' && s[1] <= 'F') lo = s[1] - 'A' + 10;
    else if (s[1] >= 'a' && s[1] <= 'f') lo = s[1] - 'a' + 10;
    else return -1;
    return (hi << 4) | lo;
}

static void url_decode(char *dst, const char *src, size_t dst_sz)
{
    size_t di = 0;
    while (*src && di < dst_sz - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int ch = url_decode_char(src + 1);
            if (ch >= 0) { dst[di++] = (char)ch; src += 3; continue; }
        }
        if (*src == '+') { dst[di++] = ' '; src++; continue; }
        dst[di++] = *src++;
    }
    dst[di] = '\0';
}

/* ── Parse form field from URL-encoded body ──────────────────── */

static bool get_field(const char *body, const char *name,
                      char *out, size_t out_sz)
{
    size_t nlen = strlen(name);
    const char *p = body;
    while ((p = strstr(p, name)) != NULL) {
        /* Make sure we match the full field name (preceded by & or start) */
        if (p != body && *(p - 1) != '&') { p += nlen; continue; }
        if (p[nlen] != '=') { p += nlen; continue; }
        p += nlen + 1;
        const char *end = strchr(p, '&');
        size_t vlen = end ? (size_t)(end - p) : strlen(p);
        if (vlen >= out_sz) vlen = out_sz - 1;
        /* Copy raw then decode */
        char raw[128];
        if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
        memcpy(raw, p, vlen);
        raw[vlen] = '\0';
        url_decode(out, raw, out_sz);
        return true;
    }
    out[0] = '\0';
    return false;
}

/* ── HTML entity-encode (for safe embedding in value="") ─────── */

static void html_escape(char *dst, const char *src, size_t dst_sz)
{
    size_t di = 0;
    while (*src && di < dst_sz - 6) {
        switch (*src) {
        case '&':  memcpy(dst+di, "&amp;", 5);  di += 5; break;
        case '<':  memcpy(dst+di, "&lt;", 4);   di += 4; break;
        case '>':  memcpy(dst+di, "&gt;", 4);   di += 4; break;
        case '"':  memcpy(dst+di, "&quot;", 6);  di += 6; break;
        case '\'': memcpy(dst+di, "&#39;", 5);   di += 5; break;
        default:   dst[di++] = *src; break;
        }
        src++;
    }
    dst[di] = '\0';
}

/* ── GET / ───────────────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    repeater_config_t cfg;
    repeater_config_load(&cfg);

    /* Escape values for HTML */
    char e_sta_ssid[128], e_sta_pass[256], e_ap_ssid[128], e_ap_pass[256];
    html_escape(e_sta_ssid, cfg.sta_ssid, sizeof(e_sta_ssid));
    html_escape(e_sta_pass, cfg.sta_pass, sizeof(e_sta_pass));
    html_escape(e_ap_ssid,  cfg.ap_ssid,  sizeof(e_ap_ssid));
    html_escape(e_ap_pass,  cfg.ap_pass,  sizeof(e_ap_pass));

    /* Render — HTML_PAGE has 6 format specifiers */
    size_t buf_len = sizeof(HTML_PAGE) + 1024;
    char *buf = malloc(buf_len);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    snprintf(buf, buf_len, HTML_PAGE,
             e_sta_ssid, e_sta_pass,
             e_ap_ssid, e_ap_pass,
             cfg.max_clients, cfg.tx_power_dbm);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    free(buf);
    return ESP_OK;
}

/* ── POST /save ──────────────────────────────────────────────── */

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[512];
    int recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[recv] = '\0';

    repeater_config_t cfg;
    repeater_config_load(&cfg);

    char tmp[128];
    if (get_field(body, "sta_ssid", tmp, sizeof(tmp)))
        strlcpy(cfg.sta_ssid, tmp, sizeof(cfg.sta_ssid));
    if (get_field(body, "sta_pass", tmp, sizeof(tmp)))
        strlcpy(cfg.sta_pass, tmp, sizeof(cfg.sta_pass));
    if (get_field(body, "ap_ssid", tmp, sizeof(tmp)))
        strlcpy(cfg.ap_ssid, tmp, sizeof(cfg.ap_ssid));
    if (get_field(body, "ap_pass", tmp, sizeof(tmp)))
        strlcpy(cfg.ap_pass, tmp, sizeof(cfg.ap_pass));
    if (get_field(body, "max_cli", tmp, sizeof(tmp))) {
        int v = atoi(tmp);
        if (v >= 1 && v <= 10) cfg.max_clients = v;
    }
    if (get_field(body, "tx_pwr", tmp, sizeof(tmp))) {
        int v = atoi(tmp);
        if (v >= 2 && v <= 20) cfg.tx_power_dbm = v;
    }

    esp_err_t err = repeater_config_save(&cfg);
    if (err != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Config saved, rebooting in 1s...");

    /* Redirect back so user sees confirmation */
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    httpd_resp_send(req, NULL, 0);

    /* Reboot after a short delay so HTTP response is sent */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

/* ── POST /reset ─────────────────────────────────────────────── */

static esp_err_t reset_post_handler(httpd_req_t *req)
{
    repeater_config_reset();

    ESP_LOGI(TAG, "Config reset, rebooting in 1s...");
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── GET /status ─────────────────────────────────────────────── */

/* Forward-declared externs from main (state, flags) */
extern volatile int      s_state;           /* repeater_state_t */
extern volatile bool     s_sta_connected;
extern volatile bool     s_forwarding_active;
extern volatile bool     s_mac_cloned;
extern esp_netif_t      *s_sta_netif;

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char json[512];
    const char *state_str;
    switch (s_state) {
        case 0:  state_str = "IDLE"; break;
        case 1:  state_str = "MAC_CHANGING"; break;
        case 2:  state_str = "BRIDGING"; break;
        case 3:  state_str = "MAC_RESTORING"; break;
        default: state_str = "UNKNOWN"; break;
    }

    /* Upstream info */
    char upstream[34] = "";
    int rssi = 0, channel = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(upstream, (char *)ap.ssid, sizeof(upstream));
        rssi = ap.rssi;
        channel = ap.primary;
    }

    /* STA MAC */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Client count */
    int clients = 0;
    wifi_sta_list_t sta_list;
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        clients = sta_list.num;
    }

    /* IP */
    char ip_str[16] = "none";
    esp_netif_ip_info_t ip_info;
    if (s_sta_netif && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK
        && ip_info.ip.addr != 0) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    int64_t uptime = esp_timer_get_time() / 1000000;

    snprintf(json, sizeof(json),
        "{\"state\":\"%s\",\"upstream\":\"%s\",\"rssi\":%d,\"channel\":%d,"
        "\"sta_mac\":\"%s\",\"cloned\":%s,\"clients\":%d,"
        "\"forwarding\":%s,\"ip\":\"%s\",\"uptime\":%lld}",
        state_str, upstream, rssi, channel,
        mac_str, s_mac_cloned ? "true" : "false", clients,
        s_forwarding_active ? "true" : "false", ip_str, (long long)uptime);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Start / Stop ────────────────────────────────────────────── */

esp_err_t repeater_httpd_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_REPEATER_HTTPD_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 4;
    /* Keep stack small — we malloc the HTML buffer */
    config.stack_size = 4096 + 1024;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = root_get_handler },
        { .uri = "/save",   .method = HTTP_POST, .handler = save_post_handler },
        { .uri = "/reset",  .method = HTTP_POST, .handler = reset_post_handler },
        { .uri = "/status", .method = HTTP_GET,  .handler = status_get_handler },
    };
    for (int i = 0; i < sizeof(uris)/sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port %d", CONFIG_REPEATER_HTTPD_PORT);
    return ESP_OK;
}

void repeater_httpd_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

#else /* CONFIG_REPEATER_HTTPD_ENABLE not set */

#include "repeater_httpd.h"

esp_err_t repeater_httpd_start(void) { return ESP_OK; }
void repeater_httpd_stop(void) { }

#endif
