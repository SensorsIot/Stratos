idf_component_register(
    SRCS "src/oled_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_event sonde_types
    PRIV_REQUIRES board_profile config_store platform_common rf_sx1276 esp_timer
)
