idf_component_register(
    SRCS "src/ota_handler.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client mqtt_ha device_id nvs_flash cjson esp_event
)
