#include "gateway.h"
#include "rest.h"
#include "events.h"
#include "json_parse.h"
#include "config/nvs_config.h"
#include "net/ca_store.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG               "gateway"
#define RECV_BUF_SIZE     16384
#define SESSION_ID_LEN    128
#define RESUME_URL_LEN    256
#define WS_PATH           "/?v=10&encoding=json"

/* Discord gateway op codes */
#define OP_DISPATCH       0
#define OP_HEARTBEAT      1
#define OP_IDENTIFY       2
#define OP_RESUME         6
#define OP_RECONNECT      7
#define OP_INVALID_SESSION 9
#define OP_HELLO          10
#define OP_HEARTBEAT_ACK  11

/* Discord intents: GUILDS | GUILD_MESSAGES | MESSAGE_CONTENT */
#define BOT_INTENTS       33281

#define GW_EV_DISCONNECTED BIT0
#define GW_EV_STOP         BIT1

static struct {
    esp_websocket_client_handle_t ws;
    char         session_id[SESSION_ID_LEN];
    char         resume_url[RESUME_URL_LEN];
    int          seq;
    int          hb_interval_ms;
    bool         hb_ack_received;
    bool         connected;
    bool         resumable;
    gateway_state_t state;
    esp_timer_handle_t hb_timer;
    char        *recv_buf;
    int          recv_len;
    int          recv_total;
    int64_t      hb_sent_us;
    int32_t      latency_ms;
    EventGroupHandle_t evg;
} s;

/* ---- Helpers ------------------------------------------------------------- */

static void send_frame(const char *json) {
    if (!s.connected || !s.ws) return;
    int r = esp_websocket_client_send_text(s.ws, json, (int)strlen(json),
                                           pdMS_TO_TICKS(3000));
    if (r < 0) ESP_LOGW(TAG, "send_frame failed: %d", r);
}

static void send_heartbeat(void) {
    char buf[48];
    if (s.seq > 0) snprintf(buf, sizeof(buf), "{\"op\":1,\"d\":%d}", s.seq);
    else            snprintf(buf, sizeof(buf), "{\"op\":1,\"d\":null}");
    s.hb_sent_us = esp_timer_get_time();
    send_frame(buf);
}

static void send_identify(void) {
    char token[96] = {0};
    nvs_config_get_bot_token(token, sizeof(token));
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"op\":2,\"d\":{"
        "\"token\":\"%s\","
        "\"intents\":%d,"
        "\"properties\":{\"os\":\"freertos\",\"browser\":\"esp32\",\"device\":\"m5stickc-plus2\"}"
        "}}",
        token, BOT_INTENTS);
    s.state = GW_STATE_IDENTIFYING;
    nvs_config_increment_identify();
    send_frame(buf);
    ESP_LOGI(TAG, "IDENTIFY sent (daily count: %d)", nvs_config_identify_count());
}

static void send_resume(void) {
    char token[96] = {0};
    nvs_config_get_bot_token(token, sizeof(token));
    char buf[320];
    snprintf(buf, sizeof(buf),
        "{\"op\":6,\"d\":{"
        "\"token\":\"%s\","
        "\"session_id\":\"%s\","
        "\"seq\":%d"
        "}}",
        token, s.session_id, s.seq);
    s.state = GW_STATE_RESUMING;
    send_frame(buf);
    ESP_LOGI(TAG, "RESUME sent (session: %.16s...)", s.session_id);
}

/* ---- Heartbeat timer ----------------------------------------------------- */

static void hb_timer_cb(void *arg) {
    if (!s.connected) return;
    /* Missed ACK → zombie connection */
    if (!s.hb_ack_received && s.hb_sent_us > 0) {
        ESP_LOGW(TAG, "heartbeat ACK missed — closing connection");
        s.connected = false;
        esp_websocket_client_close(s.ws, pdMS_TO_TICKS(1000));
        return;
    }
    s.hb_ack_received = false;
    send_heartbeat();
}

static void start_heartbeat(int interval_ms) {
    s.hb_interval_ms = interval_ms;
    s.hb_ack_received = true; /* treat as acked before first send */
    s.hb_sent_us = 0;

    esp_timer_create_args_t args = {
        .callback = hb_timer_cb,
        .name     = "hb",
    };
    if (s.hb_timer) {
        esp_timer_stop(s.hb_timer);
        esp_timer_delete(s.hb_timer);
    }
    esp_timer_create(&args, &s.hb_timer);
    int first_ms = interval_ms / 2 + (int)(esp_timer_get_time() % (interval_ms / 2));
    esp_timer_start_once(s.hb_timer, (uint64_t)first_ms * 1000);
}

static void restart_heartbeat_periodic(void) {
    if (!s.hb_timer) return;
    esp_timer_stop(s.hb_timer);
    esp_timer_start_periodic(s.hb_timer, (uint64_t)s.hb_interval_ms * 1000);
}

/* ---- Event parsing ------------------------------------------------------- */

static void parse_ready(const char *js, jsmntok_t *toks, int ntoks, int d_idx) {
    jp_str(js, toks, ntoks, d_idx, "session_id",
           s.session_id, sizeof(s.session_id));

    /* resume_gateway_url is nested one level deeper */
    char resume_url[RESUME_URL_LEN];
    if (jp_str(js, toks, ntoks, d_idx, "resume_gateway_url",
               resume_url, sizeof(resume_url))) {
        /* Append WS path */
        strlcpy(s.resume_url, resume_url, sizeof(s.resume_url));
        strlcat(s.resume_url, WS_PATH, sizeof(s.resume_url));
    }

    /* Grab application ID for slash command registration */
    int app_idx = jp_find(js, toks, ntoks, d_idx, "application");
    if (app_idx >= 0 && toks[app_idx].type == JSMN_OBJECT) {
        char app_id[32] = {0};
        if (jp_str(js, toks, ntoks, app_idx, "id", app_id, sizeof(app_id))) {
            nvs_config_set_app_id(app_id);
        }
    }

    s.state = GW_STATE_READY;
    ESP_LOGI(TAG, "READY — session %.12s... resume_url=%s",
             s.session_id, s.resume_url);
    restart_heartbeat_periodic();
}

static void parse_message_create(const char *js, jsmntok_t *toks, int ntoks, int d_idx) {
    discord_message_t msg = {0};
    jp_str(js, toks, ntoks, d_idx, "id",         msg.id,         sizeof(msg.id));
    jp_str(js, toks, ntoks, d_idx, "content",    msg.content,    sizeof(msg.content));
    jp_str(js, toks, ntoks, d_idx, "channel_id", msg.channel_id, sizeof(msg.channel_id));

    int author_idx = jp_find(js, toks, ntoks, d_idx, "author");
    if (author_idx >= 0 && toks[author_idx].type == JSMN_OBJECT) {
        jp_str(js, toks, ntoks, author_idx, "id", msg.author_id, sizeof(msg.author_id));
    }

    /* Count mentions */
    int mentions_idx = jp_find(js, toks, ntoks, d_idx, "mentions");
    if (mentions_idx >= 0 && toks[mentions_idx].type == JSMN_ARRAY)
        msg.mention_count = (uint8_t)toks[mentions_idx].size;

    /* Count URLs in content (simple heuristic: count "http") */
    const char *p = msg.content;
    while ((p = strstr(p, "http")) != NULL) { msg.url_count++; p += 4; }

    events_dispatch_message(&msg);
}

static void parse_interaction_create(const char *js, jsmntok_t *toks, int ntoks, int d_idx) {
    /* Only handle APPLICATION_COMMAND (type 2) */
    int type = 0;
    if (!jp_int(js, toks, ntoks, d_idx, "type", &type) || type != 2) return;

    discord_interaction_t ia = {0};
    jp_str(js, toks, ntoks, d_idx, "id",         ia.id,         sizeof(ia.id));
    jp_str(js, toks, ntoks, d_idx, "token",      ia.token,      sizeof(ia.token));
    jp_str(js, toks, ntoks, d_idx, "guild_id",   ia.guild_id,   sizeof(ia.guild_id));
    jp_str(js, toks, ntoks, d_idx, "channel_id", ia.channel_id, sizeof(ia.channel_id));

    int member_idx = jp_find(js, toks, ntoks, d_idx, "member");
    if (member_idx >= 0 && toks[member_idx].type == JSMN_OBJECT) {
        int user_idx = jp_find(js, toks, ntoks, member_idx, "user");
        if (user_idx >= 0 && toks[user_idx].type == JSMN_OBJECT)
            jp_str(js, toks, ntoks, user_idx, "id", ia.invoker_id, sizeof(ia.invoker_id));
    }

    int data_idx = jp_find(js, toks, ntoks, d_idx, "data");
    if (data_idx >= 0 && toks[data_idx].type == JSMN_OBJECT) {
        jp_str(js, toks, ntoks, data_idx, "name", ia.command, sizeof(ia.command));

        int opts_idx = jp_find(js, toks, ntoks, data_idx, "options");
        if (opts_idx >= 0 && toks[opts_idx].type == JSMN_ARRAY && toks[opts_idx].size > 0) {
            int n  = toks[opts_idx].size;
            int oi = opts_idx + 1;
            for (int k = 0; k < n && oi < ntoks; k++) {
                if (toks[oi].type == JSMN_OBJECT) {
                    char oname[16] = {0};
                    jp_str(js, toks, ntoks, oi, "name", oname, sizeof(oname));
                    if      (strcmp(oname, "action") == 0)
                        jp_str(js, toks, ntoks, oi, "value", ia.option_action,  sizeof(ia.option_action));
                    else if (strcmp(oname, "user")   == 0)
                        jp_str(js, toks, ntoks, oi, "value", ia.option_user_id, sizeof(ia.option_user_id));
                    else if (strcmp(oname, "reason") == 0)
                        jp_str(js, toks, ntoks, oi, "value", ia.option_reason,  sizeof(ia.option_reason));
                }
                /* Advance past this array element using its end position */
                int elem_end = toks[oi].end;
                oi++;
                while (oi < ntoks && toks[oi].start < elem_end) oi++;
            }
        }
    }

    events_dispatch_interaction(&ia);
}

static void gateway_process_message(const char *js, int len) {
    static jsmntok_t toks[JP_MAX_TOKENS];
    int ntoks = jp_tokenize(js, len, toks);
    if (ntoks < 1) {
        ESP_LOGW(TAG, "JSON parse failed (%d) for %d bytes", ntoks, len);
        return;
    }

    int op = 0;
    jp_int(js, toks, ntoks, 0, "op", &op);

    /* Update sequence number */
    int seq = 0;
    if (jp_int(js, toks, ntoks, 0, "s", &seq) && seq > 0)
        s.seq = seq;

    switch (op) {
    case OP_HELLO: {
        int d_idx = jp_find(js, toks, ntoks, 0, "d");
        int hb_ms = 41250;
        if (d_idx >= 0) jp_int(js, toks, ntoks, d_idx, "heartbeat_interval", &hb_ms);
        ESP_LOGI(TAG, "HELLO — heartbeat_interval=%d ms", hb_ms);
        start_heartbeat(hb_ms);
        /* IDENTIFY or RESUME */
        if (s.state == GW_STATE_RESUMING && s.session_id[0]) {
            send_resume();
        } else {
            if (nvs_config_identify_count() >= 950) {
                ESP_LOGE(TAG, "IDENTIFY daily limit reached — refusing to reconnect");
                xEventGroupSetBits(s.evg, GW_EV_STOP);
                break;
            }
            send_identify();
        }
        break;
    }
    case OP_DISPATCH: {
        char t[48] = {0};
        jp_str(js, toks, ntoks, 0, "t", t, sizeof(t));
        int d_idx = jp_find(js, toks, ntoks, 0, "d");
        if (strcmp(t, "READY") == 0 && d_idx >= 0)
            parse_ready(js, toks, ntoks, d_idx);
        else if (strcmp(t, "RESUMED") == 0)
            { s.state = GW_STATE_READY; ESP_LOGI(TAG, "RESUMED"); restart_heartbeat_periodic(); }
        else if (strcmp(t, "MESSAGE_CREATE") == 0 && d_idx >= 0)
            parse_message_create(js, toks, ntoks, d_idx);
        else if (strcmp(t, "INTERACTION_CREATE") == 0 && d_idx >= 0)
            parse_interaction_create(js, toks, ntoks, d_idx);
        break;
    }
    case OP_HEARTBEAT:
        send_heartbeat();
        break;
    case OP_HEARTBEAT_ACK:
        s.hb_ack_received = true;
        s.latency_ms = (int32_t)((esp_timer_get_time() - s.hb_sent_us) / 1000);
        break;
    case OP_RECONNECT:
        ESP_LOGW(TAG, "RECONNECT requested by server");
        s.resumable = true;
        s.state = GW_STATE_DISCONNECTED;
        esp_websocket_client_close(s.ws, pdMS_TO_TICKS(1000));
        break;
    case OP_INVALID_SESSION: {
        bool resumable = false;
        jp_bool(js, toks, ntoks, 0, "d", &resumable);
        s.resumable = resumable;
        if (!resumable) { s.session_id[0] = '\0'; s.seq = 0; }
        ESP_LOGW(TAG, "INVALID_SESSION (resumable=%d)", resumable);
        /* Discord recommends waiting 1-5s before re-identifying */
        vTaskDelay(pdMS_TO_TICKS(2000));
        send_identify();
        break;
    }
    default:
        break;
    }
}

/* ---- WebSocket event handler --------------------------------------------- */

static void ws_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected");
        s.connected = true;
        s.state     = GW_STATE_CONNECTING;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code != 0x01 && data->op_code != 0x00) break; /* text only */
        if (data->payload_offset == 0) {
            s.recv_len   = 0;
            s.recv_total = data->payload_len;   /* total across all fragments */
        }
        if (s.recv_buf) {
            int space = RECV_BUF_SIZE - s.recv_len - 1;
            int copy  = data->data_len < space ? data->data_len : space;
            memcpy(s.recv_buf + s.recv_len, data->data_ptr, (size_t)copy);
            s.recv_len += copy;
        }
        if (s.recv_len >= s.recv_total) {
            if (s.recv_buf) {
                s.recv_buf[s.recv_len] = '\0';
                gateway_process_message(s.recv_buf, s.recv_len);
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WS disconnected/error");
        s.connected = false;
        xEventGroupSetBits(s.evg, GW_EV_DISCONNECTED);
        break;
    }
}

/* ---- Gateway task -------------------------------------------------------- */

static esp_err_t fetch_gateway_url(char *url_out, size_t len) {
    char resp[512] = {0};
    esp_err_t err = rest_get("/gateway/bot", resp, sizeof(resp));
    if (err != ESP_OK) return err;

    static jsmntok_t toks[32];
    int ntoks = jp_tokenize(resp, (int)strlen(resp), toks);
    if (ntoks < 1) return ESP_FAIL;

    char ws_url[256] = {0};
    jp_str(resp, toks, ntoks, 0, "url", ws_url, sizeof(ws_url));
    if (ws_url[0] == '\0') return ESP_FAIL;

    strlcpy(url_out, ws_url, len);
    strlcat(url_out, WS_PATH, len);
    return ESP_OK;
}

static void connect_ws(const char *url) {
    if (s.ws) {
        esp_websocket_client_destroy(s.ws);
        s.ws = NULL;
    }

    esp_websocket_client_config_t cfg = {
        .uri         = url,
        .task_stack  = 4096,
        .buffer_size = 4096,
    };

    const char *ca_pem = ca_store_pem();
    if (ca_pem) {
        cfg.cert_pem = ca_pem;
        cfg.cert_len = ca_store_pem_len();
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    s.ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s.ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s.ws);
}

void gateway_task(void *arg) {
    (void)arg;

    s.evg      = xEventGroupCreate();
    s.recv_buf = (char *)heap_caps_malloc(RECV_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.recv_buf) {
        ESP_LOGE(TAG, "Failed to allocate recv buffer in PSRAM");
        vTaskDelete(NULL);
        return;
    }

    char gw_url[URL_LEN];
    char connect_url[URL_LEN];

    /* Fetch initial WSS URL */
    if (fetch_gateway_url(gw_url, sizeof(gw_url)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch gateway URL");
        vTaskDelete(NULL);
        return;
    }
    strlcpy(connect_url, gw_url, sizeof(connect_url));

    int backoff_ms = 1000;

    for (;;) {
        xEventGroupClearBits(s.evg, GW_EV_DISCONNECTED | GW_EV_STOP);
        s.state = GW_STATE_CONNECTING;

        /* Use resume URL if we have a valid session to resume */
        if (s.resumable && s.session_id[0] && s.resume_url[0]) {
            strlcpy(connect_url, s.resume_url, sizeof(connect_url));
            s.state = GW_STATE_RESUMING;
        } else {
            strlcpy(connect_url, gw_url, sizeof(connect_url));
        }

        ESP_LOGI(TAG, "Connecting to %s", connect_url);
        connect_ws(connect_url);

        EventBits_t bits = xEventGroupWaitBits(s.evg,
            GW_EV_DISCONNECTED | GW_EV_STOP, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & GW_EV_STOP) {
            ESP_LOGE(TAG, "Gateway stopped permanently");
            break;
        }

        /* Reconnect with exponential backoff (1s → 2s → 4s → … → 60s) */
        ESP_LOGW(TAG, "Reconnecting in %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = backoff_ms < 60000 ? backoff_ms * 2 : 60000;

        /* Refresh gateway URL occasionally (every 10 reconnects) */
        static int reconnect_count = 0;
        if (++reconnect_count % 10 == 0) {
            fetch_gateway_url(gw_url, sizeof(gw_url));
            reconnect_count = 0;
        }
    }

    if (s.ws) esp_websocket_client_destroy(s.ws);
    heap_caps_free(s.recv_buf);
    vTaskDelete(NULL);
}

bool            gateway_is_connected(void)  { return s.connected; }
int32_t         gateway_last_seq(void)      { return s.seq; }
int32_t         gateway_latency_ms(void)    { return s.latency_ms; }
gateway_state_t gateway_state(void)         { return s.state; }
