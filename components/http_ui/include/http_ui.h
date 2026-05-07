#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_http_start(void);
httpd_handle_t st_http_server(void);

#ifdef __cplusplus
}
#endif
