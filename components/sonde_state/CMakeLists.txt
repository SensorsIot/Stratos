idf_component_register(
    SRCS "src/sonde_state.c"
    INCLUDE_DIRS "include"
    REQUIRES sonde_types esp_event esp_timer
    PRIV_REQUIRES log
)
