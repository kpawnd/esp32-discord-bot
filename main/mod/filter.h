#pragma once
#include "discord/types.h"
#include "esp_err.h"

esp_err_t filter_init(void);
void      filter_check_message(const discord_message_t *msg);
