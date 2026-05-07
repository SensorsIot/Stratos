idf_component_register(
    SRCS "src/ble_nus.c"
    INCLUDE_DIRS "include"
    REQUIRES bt sonde_types esp_event
    PRIV_REQUIRES stratos_codec sonde_state log esp_timer
)
