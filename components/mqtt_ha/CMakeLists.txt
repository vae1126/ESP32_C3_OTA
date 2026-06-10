idf_component_register(
    SRCS "src/mqtt_ha.c"
    INCLUDE_DIRS "include"
    REQUIRES mqtt cjson device_id led_control #ota_handler app_update
)