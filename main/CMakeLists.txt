idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_http_server nvs_flash esp_event
    PRIV_REQUIRES esp_driver_gpio freertos
)
