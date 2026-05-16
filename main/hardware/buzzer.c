#include "buzzer.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG      "buzzer"
#define BUZ_GPIO 2
#define BUZ_FREQ 2700          /* Hz — near resonant freq of common buzzers */
#define BUZ_CH   LEDC_CHANNEL_0
#define BUZ_TMR  LEDC_TIMER_0
#define BUZ_MODE LEDC_LOW_SPEED_MODE
#define BUZ_DUTY 4096          /* 50% of 13-bit max (8191) */
#define BUZ_RES  LEDC_TIMER_13_BIT

esp_err_t buzzer_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode      = BUZ_MODE,
        .duty_resolution = BUZ_RES,
        .timer_num       = BUZ_TMR,
        .freq_hz         = BUZ_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = BUZ_MODE,
        .channel    = BUZ_CH,
        .timer_sel  = BUZ_TMR,
        .gpio_num   = BUZ_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    ESP_LOGI(TAG, "buzzer on GPIO%d @ %d Hz", BUZ_GPIO, BUZ_FREQ);
    return ESP_OK;
}

static void beep_on(void)  { ledc_set_duty(BUZ_MODE, BUZ_CH, BUZ_DUTY); ledc_update_duty(BUZ_MODE, BUZ_CH); }
static void beep_off(void) { ledc_set_duty(BUZ_MODE, BUZ_CH, 0);        ledc_update_duty(BUZ_MODE, BUZ_CH); }

void buzzer_beep(int count) {
    for (int i = 0; i < count; i++) {
        beep_on();
        vTaskDelay(pdMS_TO_TICKS(80));
        beep_off();
        if (i < count - 1) vTaskDelay(pdMS_TO_TICKS(120));
    }
}

void buzzer_long_tone(void) {
    beep_on();
    vTaskDelay(pdMS_TO_TICKS(600));
    beep_off();
}
