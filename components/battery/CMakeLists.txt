idf_component_register(
    SRCS "src/battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc
    PRIV_REQUIRES board_profile config_store log
)
