idf_component_register(
    SRCS "src/rssi_reporter.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi mqtt_ha device_id
)