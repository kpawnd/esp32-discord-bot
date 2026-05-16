#include "rest.h"
#include "config/nvs_config.h"
#include "net/ca_store.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG          "rest"
#define QUEUE_DEPTH  8
#define PATH_LEN     384
#define BODY_MAX_LEN 1024
#define RATE_RESET_MARGIN_MS 50

typedef enum { M_GET, M_POST, M_PUT, M_PATCH, M_DELETE } http_method_t;

typedef struct {
    http_method_t method;
    char          path[PATH_LEN];
    char          body[BODY_MAX_LEN]; /* empty for GET/DELETE */
    bool          with_auth;
} rest_req_t;

static QueueHandle_t    s_queue;
static SemaphoreHandle_t s_sync_sem;
static int64_t          s_rate_limit_reset_us = 0;
static esp_http_client_handle_t s_interaction_client;
static char             s_interaction_resp[512];

/* ---- HTTP event handler ------------------------------------------------- */

typedef struct { char *buf; size_t len; size_t cap; } resp_ctx_t;
static resp_ctx_t       s_interaction_ctx;

static esp_err_t http_event(esp_http_client_event_t *evt) {
    resp_ctx_t *ctx = (resp_ctx_t *)evt->user_data;
    if (!ctx) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t left = ctx->cap - ctx->len - 1;
        size_t copy = (size_t)evt->data_len < left ? (size_t)evt->data_len : left;
        memcpy(ctx->buf + ctx->len, evt->data, copy);
        ctx->len += copy;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

/* ---- Rate limit ---------------------------------------------------------- */

static void rate_limit_wait(void) {
    int64_t now = esp_timer_get_time();
    if (now < s_rate_limit_reset_us) {
        int32_t ms = (int32_t)((s_rate_limit_reset_us - now) / 1000) + RATE_RESET_MARGIN_MS;
        if (ms > 0 && ms < 30000) {
            ESP_LOGW(TAG, "rate limited — waiting %d ms", ms);
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
    }
}

static void parse_rate_limit(esp_http_client_handle_t c) {
    /* esp_http_client_get_header gives us a *pointer* into its internal buffer */
    char *rem = NULL;
    if (esp_http_client_get_header(c, "X-RateLimit-Remaining", &rem) == ESP_OK && rem) {
        if (rem[0] == '0') {
            char *after = NULL;
            if (esp_http_client_get_header(c, "X-RateLimit-Reset-After", &after) == ESP_OK && after) {
                float after_s = strtof(after, NULL);
                s_rate_limit_reset_us = esp_timer_get_time() + (int64_t)(after_s * 1e6f);
            }
        }
    }
}

static void set_common_headers(esp_http_client_handle_t client) {
    esp_http_client_set_header(client, "Content-Type",  "application/json");
    esp_http_client_set_header(client, "User-Agent",    "m5stickc-discord-bot/1.0");
}

static void apply_ca_config(esp_http_client_config_t *cfg) {
    const char *ca_pem = ca_store_pem();
    if (ca_pem) {
        cfg->cert_pem = ca_pem;
        cfg->cert_len = ca_store_pem_len();
    } else {
        cfg->crt_bundle_attach = esp_crt_bundle_attach;
    }
}

static esp_err_t interaction_client_init(void) {
    if (s_interaction_client) return ESP_OK;

    s_interaction_ctx.buf = s_interaction_resp;
    s_interaction_ctx.cap = sizeof(s_interaction_resp);
    s_interaction_ctx.len = 0;

    esp_http_client_config_t cfg = {
        .url                 = REST_BASE_URL "/gateway",
        .event_handler       = http_event,
        .user_data           = &s_interaction_ctx,
        .timeout_ms          = 6000,
        .buffer_size_tx      = 512,
        .keep_alive_enable   = true,
        .keep_alive_idle     = 10,
        .keep_alive_interval = 5,
        .keep_alive_count    = 3,
    };
    apply_ca_config(&cfg);

    s_interaction_client = esp_http_client_init(&cfg);
    if (!s_interaction_client) return ESP_ERR_NO_MEM;

    set_common_headers(s_interaction_client);
    return ESP_OK;
}

static esp_err_t interaction_request(esp_http_client_method_t method, const char *path,
                                     const char *body, int timeout_ms) {
    esp_err_t err = interaction_client_init();
    if (err != ESP_OK) return err;

    char url[PATH_LEN + sizeof(REST_BASE_URL)];
    int url_len = snprintf(url, sizeof(url), "%s%s", REST_BASE_URL, path);
    if (url_len < 0 || url_len >= (int)sizeof(url)) return ESP_ERR_INVALID_SIZE;

    s_interaction_ctx.len = 0;
    s_interaction_resp[0] = '\0';

    esp_http_client_set_url(s_interaction_client, url);
    esp_http_client_set_method(s_interaction_client, method);
    esp_http_client_set_timeout_ms(s_interaction_client, timeout_ms);
    if (body && body[0]) {
        esp_http_client_set_post_field(s_interaction_client, body, (int)strlen(body));
    } else {
        esp_http_client_set_post_field(s_interaction_client, NULL, 0);
    }
    set_common_headers(s_interaction_client);

    err = esp_http_client_perform(s_interaction_client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(s_interaction_client);
        if (status >= 400) {
            ESP_LOGW(TAG, "REQ %s -> HTTP %d: %s", path, status, s_interaction_resp);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "interaction HTTP error on %s: %s", path, esp_err_to_name(err));
        esp_http_client_cleanup(s_interaction_client);
        s_interaction_client = NULL;
    }

    return err;
}

/* ---- Core HTTP execute --------------------------------------------------- */

static esp_err_t do_request(http_method_t method, const char *path,
                             const char *body, char *out_buf, size_t out_cap,
                             bool with_auth) {
    char url[PATH_LEN + sizeof(REST_BASE_URL)];
    int url_len = snprintf(url, sizeof(url), "%s%s", REST_BASE_URL, path);
    if (url_len < 0 || url_len >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "REST URL too long: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    char auth[160] = "Bot ";
    if (with_auth) {
        nvs_config_get_bot_token(auth + 4, sizeof(auth) - 4);
        if (auth[4] == '\0') {
            ESP_LOGE(TAG, "missing Discord bot token");
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (with_auth) rate_limit_wait();

    bool free_resp = false;
    if (!out_buf) {
        out_buf = calloc(1, REST_RESP_BUF_LEN);
        if (!out_buf) return ESP_ERR_NO_MEM;
        out_cap = REST_RESP_BUF_LEN;
        free_resp = true;
    }

    resp_ctx_t ctx = { .buf = out_buf,
                       .len = 0,
                       .cap = out_cap };
    if (ctx.buf && ctx.cap > 0) ctx.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url            = url,
        .event_handler  = http_event,
        .user_data      = &ctx,
        .timeout_ms     = 8000,
        .buffer_size_tx = 512,
    };

    const char *ca_pem = ca_store_pem();
    if (ca_pem) {
        cfg.cert_pem = ca_pem;
        cfg.cert_len = ca_store_pem_len();
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        if (free_resp) free(out_buf);
        return ESP_ERR_NO_MEM;
    }

    static const esp_http_client_method_t map[] = {
        HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT,
        HTTP_METHOD_PATCH, HTTP_METHOD_DELETE
    };
    esp_http_client_set_method(client, map[method]);
    set_common_headers(client);
    if (with_auth) esp_http_client_set_header(client, "Authorization", auth);

    if (body && body[0]) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (with_auth) parse_rate_limit(client);
        if (status == 429 && with_auth) {
            /* 429 Too Many Requests: retry once after honouring reset */
            esp_http_client_cleanup(client);
            rate_limit_wait();
            ctx.len = 0;
            if (ctx.buf && ctx.cap > 0) ctx.buf[0] = '\0';
            client = esp_http_client_init(&cfg);
            if (!client) {
                if (free_resp) free(out_buf);
                return ESP_ERR_NO_MEM;
            }
            esp_http_client_set_method(client, map[method]);
            set_common_headers(client);
            if (with_auth) esp_http_client_set_header(client, "Authorization", auth);
            if (body && body[0]) esp_http_client_set_post_field(client, body, (int)strlen(body));
            err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                status = esp_http_client_get_status_code(client);
                parse_rate_limit(client);
                if (status >= 400) {
                    ESP_LOGW(TAG, "%s %s -> HTTP %d: %s",
                             (method == M_GET ? "GET" : "REQ"), path, status, ctx.buf);
                    err = ESP_FAIL;
                }
            }
        } else if (status >= 400) {
            ESP_LOGW(TAG, "%s %s -> HTTP %d: %s", (method == M_GET ? "GET" : "REQ"), path, status, ctx.buf);
            err = ESP_FAIL;
        }
    } else {
        int status = esp_http_client_get_status_code(client);
        if (status == 401) {
            ESP_LOGE(TAG, "%s %s -> HTTP 401 Unauthorized: check the bot token in config (paste the raw token, without \"Bot \")",
                     (method == M_GET ? "GET" : "REQ"), path);
        } else if (status >= 400) {
            ESP_LOGE(TAG, "%s %s -> HTTP %d: %s",
                     (method == M_GET ? "GET" : "REQ"), path, status, ctx.buf);
        } else {
            ESP_LOGE(TAG, "HTTP error on %s: %s", path, esp_err_to_name(err));
        }
    }

    esp_http_client_cleanup(client);
    if (free_resp) free(out_buf);
    return err;
}

/* ---- Public API ---------------------------------------------------------- */

esp_err_t rest_init(void) {
    s_queue    = xQueueCreate(QUEUE_DEPTH, sizeof(rest_req_t));
    s_sync_sem = xSemaphoreCreateMutex();
    if (!s_queue || !s_sync_sem) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void rest_task(void *arg) {
    (void)arg;
    static rest_req_t req;
    for (;;) {
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) == pdTRUE) {
            do_request(req.method, req.path, req.body[0] ? req.body : NULL, NULL, 0, req.with_auth);
        }
    }
}

esp_err_t rest_get(const char *path, char *out_buf, size_t bufsz) {
    xSemaphoreTake(s_sync_sem, portMAX_DELAY);
    esp_err_t err = do_request(M_GET, path, NULL, out_buf, bufsz, true);
    xSemaphoreGive(s_sync_sem);
    return err;
}

esp_err_t rest_put_sync(const char *path, const char *body) {
    xSemaphoreTake(s_sync_sem, portMAX_DELAY);
    esp_err_t err = do_request(M_PUT, path, body, NULL, 0, true);
    xSemaphoreGive(s_sync_sem);
    return err;
}

esp_err_t rest_post_sync_unauth(const char *path, const char *body) {
    return do_request(M_POST, path, body, NULL, 0, false);
}

esp_err_t rest_interaction_ack_sync(const char *path, const char *body) {
    return interaction_request(HTTP_METHOD_POST, path, body, 2800);
}

esp_err_t rest_interaction_warm(void) {
    return interaction_request(HTTP_METHOD_GET, "/gateway", NULL, 6000);
}

static esp_err_t enqueue(http_method_t method, const char *path, const char *body,
                         bool front, bool with_auth) {
    rest_req_t *req = calloc(1, sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;

    req->method = method;
    req->with_auth = with_auth;
    if (strlcpy(req->path, path, sizeof(req->path)) >= sizeof(req->path)) {
        ESP_LOGE(TAG, "REST path too long: %.80s...", path);
        free(req);
        return ESP_ERR_INVALID_SIZE;
    }
    if (body) strlcpy(req->body, body, sizeof(req->body));

    BaseType_t ok = front
        ? xQueueSendToFront(s_queue, req, pdMS_TO_TICKS(100))
        : xQueueSend(s_queue, req, pdMS_TO_TICKS(100));
    free(req);

    if (ok != pdTRUE) {
        ESP_LOGW(TAG, "REST queue full, dropping %s %s", path, body ? "" : "");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t rest_post(const char *path, const char *body)       { return enqueue(M_POST,   path, body, false, true);  }
esp_err_t rest_post_front(const char *path, const char *body) { return enqueue(M_POST,   path, body, true,  true);  }
esp_err_t rest_patch(const char *path, const char *body)      { return enqueue(M_PATCH,  path, body, false, true);  }
esp_err_t rest_patch_unauth(const char *path, const char *body) { return enqueue(M_PATCH, path, body, false, false); }
esp_err_t rest_delete(const char *path)                       { return enqueue(M_DELETE, path, NULL, false, true);  }
