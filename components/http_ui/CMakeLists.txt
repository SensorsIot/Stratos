idf_component_register(
    SRCS "src/http_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_event sonde_types
    PRIV_REQUIRES config_store platform_common esp_app_format esp_timer rf_sx1276 decoder_rs41
    EMBED_TXTFILES "web/index.html"
)
