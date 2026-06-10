idf_component_register(
    SRCS "src/wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_wifi esp_netif freertos esp_event
)