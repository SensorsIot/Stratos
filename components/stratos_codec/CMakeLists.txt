idf_component_register(
    SRCS "src/stratos_codec.c"
    INCLUDE_DIRS "include"
    REQUIRES sonde_types
    PRIV_REQUIRES config_store platform_common battery board_profile log esp_timer
)
