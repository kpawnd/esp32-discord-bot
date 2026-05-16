idf_component_register(
    SRCS
        "main.c"
        "net/wifi.c"
        "net/ca_store.c"
        "config/nvs_config.c"
        "config/web_server.c"
        "discord/json_parse.c"
        "discord/rest.c"
        "discord/gateway.c"
        "discord/events.c"
        "mod/filter.c"
        "mod/warnings.c"
        "commands/dispatcher.c"
        "hardware/display.c"
        "hardware/buttons.c"
        "hardware/buzzer.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_websocket_client
        esp_http_server
        spiffs
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_spi
        esp_timer
        esp-tls
        mbedtls
        jsmn
)

spiffs_create_partition_image(spiffs ../spiffs_image FLASH_IN_PROJECT)
