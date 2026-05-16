#pragma once
#include "esp_err.h"

esp_err_t buzzer_init(void);
void      buzzer_beep(int count);   /* short beeps: 1=single, 2=double, 3=triple */
void      buzzer_long_tone(void);   /* long warning tone */
