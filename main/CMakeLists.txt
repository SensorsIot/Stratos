idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash esp_event esp_wifi
        platform_common
        sonde_types
        config_store
        board_profile
        wifi_manager
        http_ui
        ota
        oled_ui
        button
        battery
        ble_nus
        rf_sx1276
        decoder_core
        decoder_rs41
        decoder_m10
        decoder_m20
        decoder_dfm
        sonde_state
        stratos_codec
)
