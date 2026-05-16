#include "web_server.h"
#include "nvs_config.h"
#include "net/wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG             "webserver"
#define IDLE_TIMEOUT_MS (5 * 60 * 1000) /* auto-stop after 5 minutes */
#define MAX_FORM_BODY   2048

static httpd_handle_t s_server = NULL;
static int64_t        s_last_req_us = 0;

/* ---- HTML (served from SPIFFS) via embedded string ----------------------- */
/* If SPIFFS is not mounted, fall back to this minimal inline page */
static const char *FALLBACK_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>Discord Bot Config</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:40px auto;background:#1a1a2e;color:#eee}"
    "input,select{width:100%;padding:8px;margin:4px 0 12px;background:#16213e;color:#eee;border:1px solid #0f3460;border-radius:4px}"
    "button{padding:10px 24px;background:#0f3460;color:#fff;border:none;border-radius:4px;cursor:pointer}"
    "h2{color:#e94560}.status{background:#0d1117;padding:12px;border-radius:4px;font-size:13px}"
    "</style></head><body>"
    "<h2>Discord Bot Config</h2>"
    "<form method='POST' action='/save'>"
    "<label>Bot Token:<input name='bot_token' type='password' placeholder='Raw bot token, without Bot prefix'></label>"
    "<label>WiFi SSID:<input name='wifi_ssid' placeholder='Network name'></label>"
    "<label>WiFi Password:<input name='wifi_pass' type='password' placeholder='WiFi password'></label>"
    "<label>Guild ID:<input name='guild_id' placeholder='Right-click server → Copy ID'></label>"
    "<label>Channel ID:<input name='channel_id' placeholder='Main channel ID'></label>"
    "<label>Log Channel ID:<input name='log_channel_id' placeholder='Audit log channel ID'></label>"
    "<label>Keywords (comma-separated):<input name='keywords' placeholder='spam,raid,..''></label>"
    "<label>Mention threshold:<input name='mention_thresh' type='number' value='5' min='1' max='20'></label>"
    "<label>Link threshold:<input name='link_thresh' type='number' value='3' min='1' max='10'></label>"
    "<button type='submit'>Save & Restart Bot</button>"
    "</form>"
    "<hr><div class='status' id='s'>Loading...</div>"
    "<script>function upd(){fetch('/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('s').innerHTML="
    "'Gateway: '+d.gw+'<br>Heap: '+d.heap+'KB<br>Uptime: '+d.uptime+'s<br>WiFi: '+d.rssi+'dBm';"
    "});}upd();setInterval(upd,3000);</script>"
    "</body></html>";

/* ---- URL decode helper --------------------------------------------------- */
static void url_decode(char *dst, const char *src, size_t dstlen) {
    size_t i = 0;
    while (*src && i < dstlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static const char *form_field(const char *body, const char *key, char *out, size_t outlen) {
    char search[48];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return ""; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char raw[256] = {0};
    if (len >= sizeof(raw)) len = sizeof(raw) - 1;
    memcpy(raw, p, len);
    raw[len] = '\0';
    url_decode(out, raw, outlen);
    return out;
}

/* ---- HTTP handlers ------------------------------------------------------- */

static esp_err_t handler_get_root(httpd_req_t *req) {
    s_last_req_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, FALLBACK_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#define SAVE_HTML_ERROR(msg) \
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee'>" \
    "<h2 style='color:#e94560'>Error</h2><p>" msg "</p>" \
    "<a href='/' style='color:#4db8ff'>Back</a></body></html>"

static const char *SAVE_HTML_OK =
    "<html><body style='font-family:sans-serif;background:#1a1a2e;color:#eee'>"
    "<h2 style='color:#4CAF50'>WiFi Connected!</h2>"
    "<p>Credentials verified. Saving config and restarting bot...</p>"
    "</body></html>";

static esp_err_t handler_post_save(httpd_req_t *req) {
    s_last_req_us = esp_timer_get_time();

    char body[MAX_FORM_BODY] = {0};
    size_t content_len = req->content_len;
    if (content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    if (content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < content_len) {
        int rcvd = httpd_req_recv(req, body + total, sizeof(body) - total - 1);
        if (rcvd < 0) {
            if (rcvd == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive body");
            return ESP_FAIL;
        }
        if (rcvd == 0) break;
        total += (size_t)rcvd;
    }
    body[total] = '\0';
    if (total != content_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete body");
        return ESP_FAIL;
    }

    char bot_token[128], wifi_ssid[64], wifi_pass[64];
    char guild_id[24], channel_id[24], log_channel_id[24];
    char keywords[512], mention_s[8], link_s[8];

    form_field(body, "bot_token",      bot_token,      sizeof(bot_token));
    form_field(body, "wifi_ssid",      wifi_ssid,      sizeof(wifi_ssid));
    form_field(body, "wifi_pass",      wifi_pass,      sizeof(wifi_pass));
    form_field(body, "guild_id",       guild_id,       sizeof(guild_id));
    form_field(body, "channel_id",     channel_id,     sizeof(channel_id));
    form_field(body, "log_channel_id", log_channel_id, sizeof(log_channel_id));
    form_field(body, "keywords",       keywords,       sizeof(keywords));
    form_field(body, "mention_thresh", mention_s,      sizeof(mention_s));
    form_field(body, "link_thresh",    link_s,         sizeof(link_s));

    /* Basic validation before hitting the network */
    if (wifi_ssid[0] == '\0' || bot_token[0] == '\0') {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SAVE_HTML_ERROR("WiFi SSID and Bot Token are required."),
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Test WiFi credentials — blocks up to 15 s, AP stays alive (APSTA mode) */
    ESP_LOGI(TAG, "testing WiFi credentials for SSID \"%s\"", wifi_ssid);
    if (!wifi_test_credentials(wifi_ssid, wifi_pass, 30000)) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req,
            SAVE_HTML_ERROR("WiFi connection failed &mdash; wrong SSID or password. Try again."),
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Credentials confirmed — commit to NVS and restart */
    uint8_t mention_thresh = (uint8_t)atoi(mention_s[0] ? mention_s : "5");
    uint8_t link_thresh    = (uint8_t)atoi(link_s[0]    ? link_s    : "3");

    nvs_config_save_all(bot_token, wifi_ssid, wifi_pass, guild_id,
                        channel_id, log_channel_id, keywords,
                        mention_thresh, link_thresh);

    ESP_LOGI(TAG, "config saved — WiFi verified — restarting");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVE_HTML_OK, HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_get_status(httpd_req_t *req) {
    s_last_req_us = esp_timer_get_time();

    extern bool    gateway_is_connected(void);
    extern int8_t  wifi_rssi(void);

    char resp[256];
    snprintf(resp, sizeof(resp),
        "{\"gw\":\"%s\",\"heap\":%lu,\"uptime\":%lld,\"rssi\":%d}",
        gateway_is_connected() ? "Connected" : "Disconnected",
        (unsigned long)(esp_get_free_heap_size() / 1024),
        esp_timer_get_time() / 1000000LL,
        wifi_rssi());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ---- Start / stop -------------------------------------------------------- */

esp_err_t web_server_start(void) {
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.stack_size  = 8192;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return ESP_FAIL;
    }

    httpd_uri_t root   = { .uri="/",       .method=HTTP_GET,  .handler=handler_get_root   };
    httpd_uri_t save   = { .uri="/save",   .method=HTTP_POST, .handler=handler_post_save  };
    httpd_uri_t status = { .uri="/status", .method=HTTP_GET,  .handler=handler_get_status };
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &save);
    httpd_register_uri_handler(s_server, &status);

    s_last_req_us = esp_timer_get_time();
    ESP_LOGI(TAG, "config server started on port 80");
    return ESP_OK;
}

void web_server_stop(void) {
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "config server stopped");
}

bool web_server_is_running(void) { return s_server != NULL; }
