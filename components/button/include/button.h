#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ST_BTN_SHORT = 0,
    ST_BTN_LONG  = 1,
} st_btn_event_t;

typedef void (*st_btn_handler_t)(st_btn_event_t evt, void *ctx);

esp_err_t st_button_init(st_btn_handler_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
