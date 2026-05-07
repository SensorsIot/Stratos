#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_wifi_start_ap(void);
const char *st_wifi_ssid(void);
esp_netif_t *st_wifi_ap_netif(void);

void st_captive_dns_start(void);

#ifdef __cplusplus
}
#endif
