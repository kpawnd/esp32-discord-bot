#pragma once
#include "esp_err.h"
#include "types.h"
#include <stdbool.h>

void      gateway_task(void *arg);       /* FreeRTOS task entry (core 0, pri 18) */
bool      gateway_is_connected(void);
int32_t   gateway_last_seq(void);
int32_t   gateway_latency_ms(void);     /* last heartbeat RTT */
gateway_state_t gateway_state(void);
