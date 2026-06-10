idf_component_register(
    SRCS "src/led_control.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_ledc led_strip nvs_flash
)