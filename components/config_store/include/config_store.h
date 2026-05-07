#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "sonde_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_MYCALL_MAX 9
#define CFG_UDP_HOST_MAX 32

typedef struct {
    uint8_t       board_id;
    uint32_t      freq_khz;
    sonde_type_t  sonde_type;
    char          mycall[CFG_MYCALL_MAX];
    bool          ble_on;
    bool          oled_persist;
    int32_t       freqofs_hz;

    uint8_t       rxbw_idx_rs41;
    uint8_t       rxbw_idx_m20;
    uint8_t       rxbw_idx_m10;
    uint8_t       rxbw_idx_pilot;
    uint8_t       rxbw_idx_dfm;

    uint16_t      vbat_min_mv;
    uint16_t      vbat_max_mv;
    uint8_t       vbat_type;

    bool          aprs_name;
    bool          mute;

    char          udp_log_host[CFG_UDP_HOST_MAX];
    uint16_t      udp_log_port;
} st_config_t;

esp_err_t   st_config_init(void);
st_config_t st_config_get(void);
esp_err_t   st_config_save(const st_config_t *cfg, uint32_t changed_mask);
esp_err_t   st_config_factory_reset(void);
esp_err_t   st_config_set_freq_khz(uint32_t khz);
esp_err_t   st_config_set_sonde_type(sonde_type_t t);

#ifdef __cplusplus
}
#endif
