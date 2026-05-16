#include "dispatcher.h"
#include "discord/rest.h"
#include "config/nvs_config.h"
#include "mod/warnings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

#define TAG        "commands"
#define CMD_QUEUE  8
#define ACK_QUEUE  8

static QueueHandle_t s_queue;
static QueueHandle_t s_ack_queue;

/* Slash command definitions (JSON for bulk registration) */
static const char *COMMANDS_JSON =
    "["
    "{\"name\":\"ping\",\"description\":\"Check bot latency\",\"type\":1},"
    "{\"name\":\"status\",\"description\":\"Show bot status\",\"type\":1},"
    "{\"name\":\"uptime\",\"description\":\"Show bot uptime\",\"type\":1},"
    "{\"name\":\"mod\",\"description\":\"Moderation actions\",\"type\":1,"
    "\"options\":["
      "{\"name\":\"action\",\"type\":3,\"required\":true,\"description\":\"Action\","
      "\"choices\":["
        "{\"name\":\"warn\",\"value\":\"warn\"},"
        "{\"name\":\"kick\",\"value\":\"kick\"},"
        "{\"name\":\"ban\",\"value\":\"ban\"},"
        "{\"name\":\"warnings\",\"value\":\"warnings\"},"
        "{\"name\":\"clearwarn\",\"value\":\"clearwarn\"}"
      "]},"
      "{\"name\":\"user\",\"type\":6,\"required\":false,\"description\":\"Target user\"},"
      "{\"name\":\"reason\",\"type\":3,\"required\":false,\"description\":\"Reason\"}"
    "]}"
    "]";

/* ---- Discord interaction helpers ---------------------------------------- */

/* path needs /interactions/<id>/<token>/callback or /webhooks/<app>/<token>/... */
#define IA_PATH_LEN  320
#define IA_BODY_LEN  512

static bool format_path(char *path, size_t path_len, const char *fmt,
                        const char *a, const char *b) {
    int n = snprintf(path, path_len, fmt, a, b);
    if (n < 0 || n >= (int)path_len) {
        ESP_LOGE(TAG, "interaction path truncated");
        return false;
    }
    return true;
}

esp_err_t commands_ack_interaction(const discord_interaction_t *ia) {
    if (!s_ack_queue) return ESP_ERR_INVALID_STATE;
    return xQueueSendToFront(s_ack_queue, ia, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void followup(const discord_interaction_t *ia, const char *content) {
    char app_id[24] = {0};
    nvs_config_get_app_id(app_id, sizeof(app_id));
    char path[IA_PATH_LEN], body[IA_BODY_LEN];
    if (!format_path(path, sizeof(path), "/webhooks/%s/%s/messages/@original", app_id, ia->token)) return;
    snprintf(body, sizeof(body), "{\"content\":\"%s\"}", content);
    rest_patch_unauth(path, body);
}

/* ---- Command implementations -------------------------------------------- */

static void cmd_ping(const discord_interaction_t *ia) {
    followup(ia, "Pong!");
}

static void cmd_status(const discord_interaction_t *ia) {
    extern int32_t gateway_latency_ms(void);
    extern bool    gateway_is_connected(void);
    extern int8_t  wifi_rssi(void);

    char msg[256];
    uint32_t heap = esp_get_free_heap_size();
    int      lat  = (int)gateway_latency_ms();
    int      rssi = (int)wifi_rssi();

    snprintf(msg, sizeof(msg),
        "**Status**\\n"
        "Gateway: %s\\n"
        "Latency: %d ms\\n"
        "Free heap: %lu KB\\n"
        "WiFi RSSI: %d dBm",
        gateway_is_connected() ? "Connected" : "Disconnected",
        lat,
        (unsigned long)(heap / 1024),
        rssi);
    followup(ia, msg);
}

static void cmd_uptime(const discord_interaction_t *ia) {
    int64_t us = esp_timer_get_time();
    int d = (int)(us / 86400000000LL);
    int h = (int)((us % 86400000000LL) / 3600000000LL);
    int m = (int)((us % 3600000000LL)  / 60000000LL);
    int s = (int)((us % 60000000LL)    / 1000000LL);
    char msg[80];
    snprintf(msg, sizeof(msg), "Uptime: %dd %dh %dm %ds", d, h, m, s);
    followup(ia, msg);
}

static void cmd_mod(const discord_interaction_t *ia) {
    const char *action = ia->option_action;
    const char *uid    = ia->option_user_id;
    const char *reason = ia->option_reason[0] ? ia->option_reason : "No reason provided";

    char guild_id[24] = {0};
    nvs_config_get_guild_id(guild_id, sizeof(guild_id));

    if (strcmp(action, "warnings") == 0) {
        uint8_t count = warnings_get(uid);
        char msg[80];
        snprintf(msg, sizeof(msg), "<@%s> has %d warning(s).", uid, (int)count);
        followup(ia, msg);
        return;
    }

    if (strcmp(action, "clearwarn") == 0) {
        warnings_clear(uid);
        followup(ia, "Warnings cleared.");
        return;
    }

    if (strcmp(action, "warn") == 0) {
        uint8_t count = warnings_increment(uid);
        char msg[256];
        snprintf(msg, sizeof(msg), "[WARN] <@%s> warned. Reason: %.100s (Total: %d)",
                 uid, reason, (int)count);
        followup(ia, msg);
        char log_ch[24] = {0};
        nvs_config_get_log_channel_id(log_ch, sizeof(log_ch));
        if (log_ch[0]) {
            char path[64], body[256];
            snprintf(path, sizeof(path), "/channels/%s/messages", log_ch);
            snprintf(body, sizeof(body),
                "{\"content\":\"[WARN] <@%s> warned by <@%s>: %.80s (warnings: %d)\"}",
                uid, ia->invoker_id, reason, (int)count);
            rest_post(path, body);
        }
    } else if (strcmp(action, "kick") == 0) {
        char path[96];
        snprintf(path, sizeof(path), "/guilds/%s/members/%s", guild_id, uid);
        rest_delete(path);
        char msg[256];
        snprintf(msg, sizeof(msg), "[KICK] <@%s> kicked. Reason: %.100s", uid, reason);
        followup(ia, msg);
    } else if (strcmp(action, "ban") == 0) {
        char path[96], body[64];
        snprintf(path, sizeof(path), "/guilds/%s/bans/%s", guild_id, uid);
        snprintf(body, sizeof(body), "{\"delete_message_seconds\":86400}");
        rest_put_sync(path, body);
        char msg[256];
        snprintf(msg, sizeof(msg), "[BAN] <@%s> banned. Reason: %.100s", uid, reason);
        followup(ia, msg);
    }
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t commands_init(void) {
    s_queue = xQueueCreate(CMD_QUEUE, sizeof(discord_interaction_t));
    s_ack_queue = xQueueCreate(ACK_QUEUE, sizeof(discord_interaction_t));
    return (s_queue && s_ack_queue) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t commands_enqueue(const discord_interaction_t *ia) {
    if (xQueueSend(s_queue, ia, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t commands_register_slash(void) {
    if (nvs_config_commands_registered()) {
        ESP_LOGI(TAG, "slash commands already registered");
        return ESP_OK;
    }

    char app_id[24] = {0}, guild_id[24] = {0};
    nvs_config_get_app_id(app_id, sizeof(app_id));
    nvs_config_get_guild_id(guild_id, sizeof(guild_id));

    if (app_id[0] == '\0' || guild_id[0] == '\0') {
        ESP_LOGW(TAG, "app_id or guild_id not set, deferring command registration");
        return ESP_ERR_INVALID_STATE;
    }

    char path[96];
    snprintf(path, sizeof(path), "/applications/%s/guilds/%s/commands", app_id, guild_id);
    ESP_LOGI(TAG, "registering slash commands at %s", path);
    esp_err_t err = rest_put_sync(path, COMMANDS_JSON);
    if (err == ESP_OK) nvs_config_set_commands_registered(true);
    return err;
}

void commands_task(void *arg) {
    (void)arg;
    discord_interaction_t ia;
    for (;;) {
        if (xQueueReceive(s_queue, &ia, portMAX_DELAY) == pdTRUE) {
            if      (strcmp(ia.command, "ping")   == 0) cmd_ping(&ia);
            else if (strcmp(ia.command, "status") == 0) cmd_status(&ia);
            else if (strcmp(ia.command, "uptime") == 0) cmd_uptime(&ia);
            else if (strcmp(ia.command, "mod")    == 0) cmd_mod(&ia);
            else ESP_LOGW(TAG, "unknown command: %s", ia.command);
        }
    }
}

void commands_ack_task(void *arg) {
    (void)arg;
    discord_interaction_t ia;

    int64_t last_warm_us = 0;
    int64_t start_us = esp_timer_get_time();
    esp_err_t warm_err = rest_interaction_warm();
    int32_t warm_ms = (int32_t)((esp_timer_get_time() - start_us) / 1000);
    if (warm_err == ESP_OK) {
        last_warm_us = esp_timer_get_time();
        ESP_LOGI(TAG, "interaction HTTP warmup complete in %d ms", (int)warm_ms);
    } else {
        ESP_LOGW(TAG, "interaction HTTP warmup failed in %d ms: %s",
                 (int)warm_ms, esp_err_to_name(warm_err));
    }

    for (;;) {
        if (xQueueReceive(s_ack_queue, &ia, pdMS_TO_TICKS(15000)) == pdTRUE) {
            char path[IA_PATH_LEN];
            if (!format_path(path, sizeof(path), "/interactions/%s/%s/callback", ia.id, ia.token)) {
                continue;
            }

            int64_t start_us = esp_timer_get_time();
            esp_err_t err = rest_interaction_ack_sync(path, "{\"type\":5,\"data\":{\"flags\":64}}");
            int32_t elapsed_ms = (int32_t)((esp_timer_get_time() - start_us) / 1000);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "interaction ACK failed in %d ms: %s",
                         (int)elapsed_ms, esp_err_to_name(err));
            } else if (elapsed_ms > 2500) {
                ESP_LOGW(TAG, "interaction ACK was slow: %d ms", (int)elapsed_ms);
            }

            if (err == ESP_OK) {
                last_warm_us = esp_timer_get_time();
                commands_enqueue(&ia);
            }
        } else {
            int64_t now = esp_timer_get_time();
            if (now - last_warm_us > 20000000LL) {
                int64_t warm_start_us = now;
                esp_err_t err = rest_interaction_warm();
                int32_t elapsed_ms = (int32_t)((esp_timer_get_time() - warm_start_us) / 1000);
                if (err == ESP_OK) {
                    last_warm_us = esp_timer_get_time();
                    if (elapsed_ms > 1000) {
                        ESP_LOGI(TAG, "interaction HTTP rewarmed in %d ms", (int)elapsed_ms);
                    }
                } else {
                    ESP_LOGW(TAG, "interaction HTTP rewarm failed in %d ms: %s",
                             (int)elapsed_ms, esp_err_to_name(err));
                }
            }
        }
    }
}
