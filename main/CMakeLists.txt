idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES device_id wifi_manager led_control mqtt_ha ota_handler rssi_reporter
)