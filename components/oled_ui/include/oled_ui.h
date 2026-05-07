#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t st_oled_init(void);
void      st_oled_set_power(bool on);
bool      st_oled_is_on(void);
void      st_oled_clear(void);
void      st_oled_render_boot(const char *ssid, const char *version);
void      st_oled_start_renderer(void);

#ifdef __cplusplus
}
#endif
