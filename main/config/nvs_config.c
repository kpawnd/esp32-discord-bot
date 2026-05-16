#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>

#define TAG       "nvs_cfg"
#define NVS_NS    "discord"

static nvs_handle_t s_nvs;

esp_err_t nvs_config_load(void) {
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return err;
}

static esp_err_t get_str(const char *key, char *buf, size_t len) {
    size_t sz = len;
    esp_err_t err = nvs_get_str(s_nvs, key, buf, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) { buf[0] = '\0'; return ESP_OK; }
    return err;
}

static esp_err_t set_str(const char *key, const char *val) {
    esp_err_t err = nvs_set_str(s_nvs, key, val);
    if (err == ESP_OK) nvs_commit(s_nvs);
    return err;
}

static void normalize_bot_token(const char *src, char *dst, size_t dst_len) {
    if (dst_len == 0) return;

    while (*src && (unsigned char)*src <= ' ') src++;

    if (strncasecmp(src, "Bot", 3) == 0 && (unsigned char)src[3] <= ' ') {
        src += 3;
        while (*src && (unsigned char)*src <= ' ') src++;
    }

    size_t len = strlen(src);
    while (len > 0 && (unsigned char)src[len - 1] <= ' ') len--;
    if (len >= dst_len) len = dst_len - 1;

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static esp_err_t get_bot_token(char *buf, size_t len) {
    char raw[192] = {0};
    esp_err_t err = get_str("bot_token", raw, sizeof(raw));
    if (err != ESP_OK) {
        if (len > 0) buf[0] = '\0';
        return err;
    }
    normalize_bot_token(raw, buf, len);
    return ESP_OK;
}

bool nvs_config_has_token(void) {
    char token[192];
    return get_bot_token(token, sizeof(token)) == ESP_OK && token[0] != '\0';
}

esp_err_t nvs_config_get_bot_token(char *buf, size_t len)      { return get_bot_token(buf, len); }
esp_err_t nvs_config_get_app_id(char *buf, size_t len)         { return get_str("app_id",        buf, len); }
esp_err_t nvs_config_set_app_id(const char *id)                { return set_str("app_id",        id);       }
esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len)      { return get_str("wifi_ssid",     buf, len); }
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len)      { return get_str("wifi_pass",     buf, len); }
esp_err_t nvs_config_get_guild_id(char *buf, size_t len)       { return get_str("guild_id",      buf, len); }
esp_err_t nvs_config_get_channel_id(char *buf, size_t len)     { return get_str("channel_id",    buf, len); }
esp_err_t nvs_config_get_log_channel_id(char *buf, size_t len) { return get_str("log_chan_id",   buf, len); }
esp_err_t nvs_config_get_keyword_list(char *buf, size_t len)   { return get_str("keywords",      buf, len); }

uint8_t nvs_config_get_mention_threshold(void) {
    uint8_t v = 5;
    nvs_get_u8(s_nvs, "mention_thresh", &v);
    return v;
}

uint8_t nvs_config_get_link_threshold(void) {
    uint8_t v = 3;
    nvs_get_u8(s_nvs, "link_thresh", &v);
    return v;
}

bool nvs_config_commands_registered(void) {
    uint8_t v = 0;
    nvs_get_u8(s_nvs, "cmds_reg", &v);
    return v != 0;
}

esp_err_t nvs_config_set_commands_registered(bool v) {
    esp_err_t err = nvs_set_u8(s_nvs, "cmds_reg", v ? 1 : 0);
    if (err == ESP_OK) nvs_commit(s_nvs);
    return err;
}

uint16_t nvs_config_identify_count(void) {
    uint16_t count = 0;
    uint32_t day   = 0;
    nvs_get_u16(s_nvs, "id_count", &count);
    nvs_get_u32(s_nvs, "id_day",   &day);
    /* reset counter if it's a new day (unix day number) */
    uint32_t today = (uint32_t)(esp_timer_get_time() / 1000000 / 86400);
    if (day != today) {
        count = 0;
        nvs_set_u16(s_nvs, "id_count", 0);
        nvs_set_u32(s_nvs, "id_day",   today);
        nvs_commit(s_nvs);
    }
    return count;
}

esp_err_t nvs_config_increment_identify(void) {
    uint16_t count = nvs_config_identify_count() + 1;
    esp_err_t err  = nvs_set_u16(s_nvs, "id_count", count);
    if (err == ESP_OK) nvs_commit(s_nvs);
    return err;
}

esp_err_t nvs_config_save_all(const char *token, const char *wifi_ssid,
                               const char *wifi_pass, const char *guild_id,
                               const char *channel_id, const char *log_channel_id,
                               const char *keywords, uint8_t mention_thresh,
                               uint8_t link_thresh) {
    char clean_token[192];
    normalize_bot_token(token, clean_token, sizeof(clean_token));

    nvs_set_str(s_nvs, "bot_token",    clean_token);
    nvs_set_str(s_nvs, "wifi_ssid",    wifi_ssid);
    nvs_set_str(s_nvs, "wifi_pass",    wifi_pass);
    nvs_set_str(s_nvs, "guild_id",     guild_id);
    nvs_set_str(s_nvs, "channel_id",   channel_id);
    nvs_set_str(s_nvs, "log_chan_id",  log_channel_id);
    nvs_set_str(s_nvs, "keywords",     keywords);
    nvs_set_u8(s_nvs,  "mention_thresh", mention_thresh);
    nvs_set_u8(s_nvs,  "link_thresh",    link_thresh);
    nvs_set_u8(s_nvs,  "cmds_reg",       0); /* force re-registration after config change */
    return nvs_commit(s_nvs);
}
