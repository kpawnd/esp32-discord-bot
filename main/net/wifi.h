#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t wifi_init(void);
void      wifi_wait_connected(void);
bool      wifi_is_connected(void);
bool      wifi_is_ap_mode(void);    /* true when running as SoftAP (no saved WiFi creds) */
int8_t    wifi_rssi(void);
bool      wifi_test_credentials(const char *ssid, const char *pass, uint32_t timeout_ms);
