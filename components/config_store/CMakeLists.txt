idf_component_register(
    SRCS "src/config_store.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_event sonde_types
)
