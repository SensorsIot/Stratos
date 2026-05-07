#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_ota_register(httpd_handle_t srv);

#ifdef __cplusplus
}
#endif
