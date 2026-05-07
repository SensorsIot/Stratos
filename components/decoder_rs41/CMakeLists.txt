idf_component_register(
    SRCS "src/decoder_rs41.c"
    INCLUDE_DIRS "include"
    REQUIRES decoder_core rf_sx1276 sonde_types
    PRIV_REQUIRES log esp_timer
)
