#include "filter.h"
#include "warnings.h"
#include "config/nvs_config.h"
#include "discord/rest.h"
#include "hardware/buzzer.h"
#include "esp_log.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define TAG           "filter"
#define MAX_KEYWORDS  32
#define KEYWORD_LEN   24

static char s_keywords[MAX_KEYWORDS][KEYWORD_LEN];
static int  s_kw_count = 0;
static uint8_t s_mention_thresh = 5;
static uint8_t s_link_thresh    = 3;

static char s_log_channel[24] = {0};

esp_err_t filter_init(void) {
    warnings_init();

    /* Load keyword list from NVS */
    char kw_list[512] = {0};
    nvs_config_get_keyword_list(kw_list, sizeof(kw_list));
    nvs_config_get_log_channel_id(s_log_channel, sizeof(s_log_channel));
    s_mention_thresh = nvs_config_get_mention_threshold();
    s_link_thresh    = nvs_config_get_link_threshold();

    s_kw_count = 0;
    char *tok = strtok(kw_list, ",");
    while (tok && s_kw_count < MAX_KEYWORDS) {
        /* trim leading space */
        while (*tok == ' ') tok++;
        strlcpy(s_keywords[s_kw_count++], tok, KEYWORD_LEN);
        tok = strtok(NULL, ",");
    }
    ESP_LOGI(TAG, "loaded %d keywords, mention_thresh=%d, link_thresh=%d",
             s_kw_count, s_mention_thresh, s_link_thresh);
    return ESP_OK;
}

static bool keyword_match(const char *content) {
    for (int i = 0; i < s_kw_count; i++) {
        if (strcasestr(content, s_keywords[i]) != NULL) return true;
    }
    return false;
}

static void delete_message(const char *channel_id, const char *msg_id) {
    char path[80];
    snprintf(path, sizeof(path), "/channels/%s/messages/%s", channel_id, msg_id);
    rest_delete(path);
}

static void log_action(const char *author_id, const char *reason, uint8_t warn_count) {
    if (s_log_channel[0] == '\0') return;
    char path[64], body[256];
    snprintf(path, sizeof(path), "/channels/%s/messages", s_log_channel);
    snprintf(body, sizeof(body),
        "{\"content\":\"⚠️ Auto-mod: <@%s> — %s (warnings: %d)\"}",
        author_id, reason, warn_count);
    rest_post(path, body);
}

void filter_check_message(const discord_message_t *msg) {
    bool violated = false;
    const char *reason = NULL;

    if (msg->mention_count >= s_mention_thresh) {
        violated = true;
        reason   = "mention spam";
    } else if (msg->url_count >= s_link_thresh) {
        violated = true;
        reason   = "link spam";
    } else if (s_kw_count > 0 && keyword_match(msg->content)) {
        violated = true;
        reason   = "banned keyword";
    }

    if (!violated) return;

    ESP_LOGI(TAG, "violation by %s: %s", msg->author_id, reason);
    delete_message(msg->channel_id, msg->id);
    uint8_t warns = warnings_increment(msg->author_id);
    log_action(msg->author_id, reason, warns);
    buzzer_beep(2);

    if (warns >= 3) buzzer_long_tone();
}
