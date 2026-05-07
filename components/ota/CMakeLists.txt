idf_component_register(
    SRCS "src/st_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server app_update
    PRIV_REQUIRES esp_app_format http_ui config_store log esp_system
)
