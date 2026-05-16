#include "wifi.h"
#include "config/nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

#define TAG                "wifi"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5
#define AP_SSID            "DiscordBot-Setup"
#define AP_PASS            "discordbot"

static EventGroupHandle_t s_wifi_eg;
static int          s_retries   = 0;
static bool         s_connected = false;
static bool         s_ap_mode   = false;
static bool         s_testing   = false;
static esp_netif_t *s_sta_netif = NULL;

/* ---- STA event handlers -------------------------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_START) {
        if (!s_testing) esp_wifi_connect(); /* test mode uses explicit connect after setting config */
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = (wifi_event_sta_disconnected_t *)data;
        if (e) ESP_LOGW(TAG, "STA disconnected: reason=%d", (int)e->reason);
        s_connected = false;
        if (s_testing) {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT); /* fail fast — no retry during test */
        } else if (s_retries < MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "reconnecting (%d/%d)", s_retries, MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(1000u << (unsigned)s_retries));
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client connected to AP");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base,
                        int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries   = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

/* ---- SoftAP first-boot config mode --------------------------------------- */

static esp_err_t start_ap(void) {
    s_ap_mode = true;
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = AP_SSID,
            .ssid_len        = (uint8_t)strlen(AP_SSID),
            .password        = AP_PASS,
            .max_connection  = 3,
            .authmode        = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: SSID=\"%s\" pass=\"%s\" — http://192.168.4.1", AP_SSID, AP_PASS);
    return ESP_OK;
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t wifi_init(void) {
    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_ip_event,   NULL));

    char ssid[64] = {0};
    nvs_config_get_wifi_ssid(ssid, sizeof(ssid));

    if (ssid[0] == '\0') {
        /* No credentials → SoftAP config mode */
        return start_ap();
    }

    /* Normal STA mode */
    char pass[64] = {0};
    nvs_config_get_wifi_pass(pass, sizeof(pass));

    esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, connecting to \"%s\"", ssid);
    return ESP_OK;
}

void wifi_wait_connected(void) {
    if (s_ap_mode) return; /* AP mode has no "connected" state to wait for */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));
    if (bits & WIFI_FAIL_BIT)
        ESP_LOGW(TAG, "WiFi connection failed — continuing anyway");
}

bool   wifi_is_connected(void) { return s_connected; }
bool   wifi_is_ap_mode(void)   { return s_ap_mode;   }

int8_t wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool wifi_test_credentials(const char *ssid, const char *pass, uint32_t timeout_ms) {
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();

    xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retries = 0;
    s_testing = true; /* inhibit auto-connect on STA_START and retry on disconnect */

    /* APSTA keeps the config AP alive while testing STA credentials */
    esp_wifi_set_mode(WIFI_MODE_APSTA); /* triggers STA_START — auto-connect is inhibited */

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid,     ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect(); /* explicit connect with correct credentials */

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    s_testing = false;
    bool ok = (bits & WIFI_CONNECTED_BIT) != 0;

    if (!ok) {
        /* Revert to AP-only so config server stays up for retry */
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_AP);
        s_retries = 0;
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        ESP_LOGW(TAG, "credential test failed for SSID \"%s\"", ssid);
    } else {
        ESP_LOGI(TAG, "credential test passed for SSID \"%s\"", ssid);
    }
    return ok;
}
