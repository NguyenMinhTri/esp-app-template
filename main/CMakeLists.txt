idf_component_register(
    SRC_DIRS .
    INCLUDE_DIRS .
    REQUIRES 
        freertos
        nvs_flash
        log
        esp32
        esp_wifi
        app_update
        double_reset 
        status_led
        wps_config
        wifi_reconnect
)
