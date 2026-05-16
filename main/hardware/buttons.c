#include "buttons.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <string.h>

#define TAG      "buttons"
#define BTN_A_IO 37
#define BTN_B_IO 39
#define LONG_PRESS_MS 2000
#define DEBOUNCE_MS   50

static QueueHandle_t s_evt_queue;

typedef struct { int gpio; int64_t press_us; bool pressed; } btn_state_t;
static btn_state_t s_state[2];

static void IRAM_ATTR btn_isr(void *arg) {
    int gpio = (int)(intptr_t)arg;
    int idx  = (gpio == BTN_A_IO) ? 0 : 1;
    int level = gpio_get_level(gpio);

    /* esp_timer_get_time not ISR-safe on all versions, use portGET_RUN_TIME_COUNTER_VALUE or pass level */
    /* Store press time via a simpler counter approach */
    if (level == 0) {
        /* Button pressed (active low) */
        s_state[idx].press_us = (int64_t)xTaskGetTickCountFromISR() * (1000000 / configTICK_RATE_HZ);
        s_state[idx].pressed = true;
    } else {
        if (!s_state[idx].pressed) return;
        s_state[idx].pressed = false;

        /* Button released */
        int64_t held_us = ((int64_t)xTaskGetTickCountFromISR() * (1000000 / configTICK_RATE_HZ))
                          - s_state[idx].press_us;
        if (held_us < (int64_t)DEBOUNCE_MS * 1000) return; /* debounce */

        button_event_t evt = {
            .id   = (button_id_t)idx,
            .type = held_us >= (int64_t)LONG_PRESS_MS * 1000
                    ? BTN_LONG_PRESS : BTN_SHORT_PRESS,
        };
        BaseType_t woken = pdFALSE;
        xQueueSendFromISR(s_evt_queue, &evt, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

esp_err_t buttons_init(void) {
    s_evt_queue = xQueueCreate(4, sizeof(button_event_t));
    if (!s_evt_queue) return ESP_ERR_NO_MEM;

    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BTN_A_IO) | BIT64(BTN_B_IO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_A_IO, btn_isr, (void *)(intptr_t)BTN_A_IO));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN_B_IO, btn_isr, (void *)(intptr_t)BTN_B_IO));

    ESP_LOGI(TAG, "buttons initialized (A=GPIO%d, B=GPIO%d)", BTN_A_IO, BTN_B_IO);
    return ESP_OK;
}

bool buttons_get_event(button_event_t *evt, TickType_t wait_ticks) {
    return xQueueReceive(s_evt_queue, evt, wait_ticks) == pdTRUE;
}
