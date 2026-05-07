#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "sonde_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sonde_type_t type;
    const char  *name;
    esp_err_t  (*start)(QueueHandle_t byte_q);
    esp_err_t  (*stop)(void);
} decoder_vtable_t;

esp_err_t   decoder_core_register(const decoder_vtable_t *v);
esp_err_t   decoder_core_set_active(sonde_type_t t, QueueHandle_t byte_q);
sonde_type_t decoder_core_active(void);
void        decoder_core_publish_frame(const sonde_frame_t *f);

#ifdef __cplusplus
}
#endif
