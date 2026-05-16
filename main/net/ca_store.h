#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

esp_err_t   ca_store_init(void);
bool        ca_store_has_custom(void);
const char *ca_store_pem(void);
size_t      ca_store_pem_len(void);
