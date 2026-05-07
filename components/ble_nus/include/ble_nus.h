#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_ble_init(const char *device_name);
void      st_ble_start_advertising(void);

#ifdef __cplusplus
}
#endif
