#pragma once
#include "discord/types.h"
#include "esp_err.h"

esp_err_t commands_init(void);
void      commands_task(void *arg);   /* FreeRTOS task entry (core 1, pri 12) */
void      commands_ack_task(void *arg);
esp_err_t commands_enqueue(const discord_interaction_t *ia);
esp_err_t commands_ack_interaction(const discord_interaction_t *ia);
esp_err_t commands_register_slash(void); /* one-shot on first boot */
