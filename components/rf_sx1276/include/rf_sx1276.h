#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "sonde_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sonde_type_t type;
    uint32_t     bitrate_bps;
    uint32_t     freq_dev_hz;
    uint32_t     rxbw_hz;
    uint8_t      sync_word[8];
    uint8_t      sync_len;
} rf_profile_t;

esp_err_t   st_rf_init(void);
esp_err_t   st_rf_set_freq_hz(uint32_t hz);
esp_err_t   st_rf_apply_profile(const rf_profile_t *p);
esp_err_t   st_rf_start_rx(void);
esp_err_t   st_rf_stop(void);
int16_t     st_rf_rssi_dbm(void);
uint32_t    st_rf_byte_count(void);   /* total bytes drained from FIFO since boot */
uint32_t    st_rf_sync_count(void);   /* total observed SyncAddressMatch events since boot */
QueueHandle_t st_rf_byte_queue(void);

const rf_profile_t *st_rf_profile_for(sonde_type_t t);

#ifdef __cplusplus
}
#endif
