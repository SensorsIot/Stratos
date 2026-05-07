idf_component_register(
    SRCS
        "src/wifi_manager.c"
        "src/captive_dns.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event
    PRIV_REQUIRES log lwip
)
