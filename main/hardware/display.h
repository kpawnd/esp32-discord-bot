#pragma once
#include "esp_err.h"
#include <stdbool.h>

esp_err_t display_init(void);
void      display_show_config_mode(const char *ip_addr);
void      display_update(void);    /* call periodically from hardware_task */
void      hardware_task(void *arg); /* FreeRTOS task entry (core 1, pri 10) */

/* Status fields updated by other modules */
void display_set_gateway_status(bool connected, const char *state_str);
void display_set_last_event(const char *event);
void display_set_heap_kb(uint32_t kb);
void display_set_rssi(int8_t rssi);
