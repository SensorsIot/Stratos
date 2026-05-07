idf_component_register(
    SRCS
        "src/version.c"
        "src/logging.c"
        "src/watchdog.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_app_format esp_event esp_timer
    PRIV_REQUIRES log esp_system lwip
)
