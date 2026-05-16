#pragma once
#include "esp_err.h"
#include <stdint.h>

#define WARN_TABLE_SIZE 64   /* users tracked concurrently */

esp_err_t warnings_init(void);
uint8_t   warnings_get(const char *user_id);
uint8_t   warnings_increment(const char *user_id);
void      warnings_clear(const char *user_id);
