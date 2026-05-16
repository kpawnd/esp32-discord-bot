#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t nvs_config_load(void);

/* credentials */
bool      nvs_config_has_token(void);
esp_err_t nvs_config_get_bot_token(char *buf, size_t len);
esp_err_t nvs_config_get_app_id(char *buf, size_t len);
esp_err_t nvs_config_set_app_id(const char *id);

/* wifi */
esp_err_t nvs_config_get_wifi_ssid(char *buf, size_t len);
esp_err_t nvs_config_get_wifi_pass(char *buf, size_t len);

/* guild / channel */
esp_err_t nvs_config_get_guild_id(char *buf, size_t len);
esp_err_t nvs_config_get_channel_id(char *buf, size_t len);
esp_err_t nvs_config_get_log_channel_id(char *buf, size_t len);

/* moderation */
esp_err_t nvs_config_get_keyword_list(char *buf, size_t len);
uint8_t   nvs_config_get_mention_threshold(void);
uint8_t   nvs_config_get_link_threshold(void);

/* slash command registration */
bool      nvs_config_commands_registered(void);
esp_err_t nvs_config_set_commands_registered(bool v);

/* daily IDENTIFY guard */
uint16_t  nvs_config_identify_count(void);
esp_err_t nvs_config_increment_identify(void);

/* write all settings at once (from web UI POST) */
esp_err_t nvs_config_save_all(const char *token, const char *wifi_ssid,
                               const char *wifi_pass, const char *guild_id,
                               const char *channel_id, const char *log_channel_id,
                               const char *keywords, uint8_t mention_thresh,
                               uint8_t link_thresh);
