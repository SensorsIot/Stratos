idf_component_register(
    SRCS "src/rf_sx1276.c"
    INCLUDE_DIRS "include"
    REQUIRES driver sonde_types
    PRIV_REQUIRES board_profile log esp_common esp_timer
)
