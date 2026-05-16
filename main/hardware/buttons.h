#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>

typedef enum { BTN_A = 0, BTN_B = 1 } button_id_t;
typedef enum { BTN_SHORT_PRESS = 0, BTN_LONG_PRESS = 1 } button_event_type_t;

typedef struct {
    button_id_t        id;
    button_event_type_t type;
} button_event_t;

esp_err_t buttons_init(void);
bool      buttons_get_event(button_event_t *evt, TickType_t wait_ticks);
