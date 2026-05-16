#pragma once
#include "esp_err.h"
#include <stddef.h>

#define REST_RESP_BUF_LEN 2048
#define REST_BASE_URL     "https://discord.com/api/v10"

esp_err_t rest_init(void);
void      rest_task(void *arg); /* FreeRTOS task entry */

/* Synchronous — blocks until response received or error */
esp_err_t rest_get(const char *path, char *out_buf, size_t bufsz);
esp_err_t rest_put_sync(const char *path, const char *body);
esp_err_t rest_post_sync_unauth(const char *path, const char *body);
esp_err_t rest_interaction_ack_sync(const char *path, const char *body);
esp_err_t rest_interaction_warm(void);

/* Asynchronous — enqueues and returns immediately */
esp_err_t rest_post(const char *path, const char *body);
esp_err_t rest_post_front(const char *path, const char *body);
esp_err_t rest_patch(const char *path, const char *body);
esp_err_t rest_patch_unauth(const char *path, const char *body);
esp_err_t rest_delete(const char *path);
