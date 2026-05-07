#pragma once

#include "esp_err.h"
#include "sonde_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t           st_state_init(void);
const sonde_frame_t *st_state_last(void);
sonde_state_t       st_state_current(void);

#ifdef __cplusplus
}
#endif
