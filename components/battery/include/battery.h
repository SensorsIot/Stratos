#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_battery_init(void);
int       st_battery_mv(void);
int       st_battery_pct(void);

#ifdef __cplusplus
}
#endif
