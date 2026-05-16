#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/apps/sntp.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>

#include "net/wifi.h"
#include "net/ca_store.h"
#include "config/nvs_config.h"
#include "config/web_server.h"
#include "discord/gateway.h"
#include "discord/rest.h"
#include "commands/dispatcher.h"
#include "mod/filter.h"
#include "hardware/display.h"
#include "hardware/buttons.h"
#include "hardware/buzzer.h"

#define TAG "main"

static void log_heap(void) {
    ESP_LOGI(TAG, "Internal free: %lu KB | PSRAM free: %lu KB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024));
}

static void enter_config_mode(void) {
    /* Determine which IP to show: AP mode = 192.168.4.1, STA mode = DHCP lease */
    char ip_str[16] = "192.168.4.1";

    if (!wifi_is_ap_mode()) {
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_get_ip_info(netif, &ip_info);
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    ESP_LOGW(TAG, "Config mode — visit http://%s", ip_str);
    ESP_ERROR_CHECK(web_server_start());
    display_show_config_mode(ip_str);

    /* web_server POST /save calls esp_restart() — block here until it does */
    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}

static bool time_is_valid(const struct tm *ti) {
    return ti->tm_year >= (2024 - 1900);
}

static void log_time(const char *label) {
    time_t now = 0;
    struct tm timeinfo = {0};
    char buf[32] = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "%s: %s", label, buf);
}

static int month_from_str(const char *s) {
    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    for (int i = 0; i < 12; i++) {
        if (strncmp(s, months[i], 3) == 0) return i;
    }
    return 0;
}

static void set_time_from_build(void) {
    struct tm tm = {0};
    tm.tm_year = atoi(__DATE__ + 7) - 1900;
    tm.tm_mon  = month_from_str(__DATE__);
    tm.tm_mday = atoi(__DATE__ + 4);
    tm.tm_hour = atoi(__TIME__ + 0);
    tm.tm_min  = atoi(__TIME__ + 3);
    tm.tm_sec  = atoi(__TIME__ + 6);
    time_t t = mktime(&tm);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void sync_time(void) {
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);
    if (time_is_valid(&timeinfo)) return;

    log_time("Time before sync");
    ESP_LOGI(TAG, "SNTP: syncing time");
    setenv("TZ", "UTC0", 1);
    tzset();
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    const int retry_max = 30;
    for (int retry = 1; retry <= retry_max; retry++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
        if (time_is_valid(&timeinfo)) break;
    }

    if (!time_is_valid(&timeinfo)) {
        ESP_LOGW(TAG, "SNTP: time not set; using build time as fallback");
        set_time_from_build();
    }
    log_time("Time after sync");
}

static void slash_register_task(void *arg) {
    (void)arg;

    for (int tries = 0; tries < 30; tries++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (gateway_state() == GW_STATE_READY) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_err_t err = commands_register_slash();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "slash command registration deferred/failed: %s",
                         esp_err_to_name(err));
            }
            break;
        }
    }

    vTaskDelete(NULL);
}

void app_main(void) {
    /* --- NVS ------------------------------------------------------------ */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(nvs_config_load());

    /* --- Hardware init -------------------------------------------------- */
    ESP_ERROR_CHECK(buzzer_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(buttons_init());

    log_heap();

    /* --- WiFi:
     *   No SSID in NVS → wifi_init() starts SoftAP "DiscordBot-Setup"
     *   SSID present    → connects as STA in normal bot mode               */
    ESP_ERROR_CHECK(wifi_init());

    if (wifi_is_ap_mode()) {
        /* First boot / unconfigured: enter config mode over the AP */
        enter_config_mode();
        /* Never returns — POST /save calls esp_restart() */
    }

    wifi_wait_connected();
    log_heap();

    /* --- Second check: WiFi connected but no token (edge case) ---------- */
    if (!nvs_config_has_token()) {
        ESP_LOGW(TAG, "WiFi connected but no token — config mode over STA");
        enter_config_mode();
    }

    sync_time();
    esp_err_t ca_err = ca_store_init();
    if (ca_err != ESP_OK) {
        ESP_LOGW(TAG, "CA store init failed: %s", esp_err_to_name(ca_err));
    }

    /* --- Bot subsystems ------------------------------------------------- */
    ESP_ERROR_CHECK(rest_init());
    ESP_ERROR_CHECK(commands_init());
    ESP_ERROR_CHECK(filter_init());

    /* --- Launch tasks --------------------------------------------------- */
    xTaskCreatePinnedToCore(gateway_task,  "gateway",  8192, NULL, 18, NULL, 0);
    xTaskCreatePinnedToCore(rest_task,     "rest",     8192, NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(commands_ack_task, "cmd_ack", 8192, NULL, 17, NULL, 1);
    xTaskCreatePinnedToCore(commands_task, "commands", 8192, NULL, 12, NULL, 1);
    xTaskCreatePinnedToCore(hardware_task, "hardware", 6144, NULL, 10, NULL, 1);
    if (xTaskCreatePinnedToCore(slash_register_task, "slash_reg", 8192, NULL, 11, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to create slash registration task");
    }

    log_heap();

    /* --- Bring-up gate 7: heap floor ------------------------------------ */
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal_free < 40 * 1024) {
        ESP_LOGE(TAG, "HEAP FLOOR BREACHED: %lu KB internal free — review TLS config",
                 (unsigned long)(internal_free / 1024));
    } else {
        ESP_LOGI(TAG, "Heap gate PASSED: %lu KB internal free",
                 (unsigned long)(internal_free / 1024));
    }
}
