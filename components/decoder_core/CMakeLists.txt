idf_component_register(
    SRCS "src/decoder_core.c"
    INCLUDE_DIRS "include"
    REQUIRES sonde_types esp_event esp_timer
    PRIV_REQUIRES log
)
