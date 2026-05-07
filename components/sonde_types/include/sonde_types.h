#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SONDE_STATE_NO_SIGNAL = 0,
    SONDE_STATE_NAME_ONLY = 1,
    SONDE_STATE_TRACKING  = 2,
} sonde_state_t;

typedef enum {
    SONDE_TYPE_UNKNOWN = 0,
    SONDE_TYPE_RS41    = 1,
    SONDE_TYPE_M20     = 2,
    SONDE_TYPE_M10     = 3,
    SONDE_TYPE_PILOT   = 4,
    SONDE_TYPE_DFM     = 5,
} sonde_type_t;

typedef struct {
    sonde_state_t state;
    sonde_type_t  type;
    char          name[16];
    double        lat;
    double        lon;
    int32_t       alt_m;
    float         h_vel_kmh;
    float         v_vel_ms;
    int16_t       rssi_dbm;
    int32_t       afc_hz;
    bool          bk_active;
    int32_t       bk_remaining_s;
    int64_t       monotonic_us;
} sonde_frame_t;

ESP_EVENT_DECLARE_BASE(SONDE_EVENT);

typedef enum {
    SONDE_EVT_FRAME = 0,
    SONDE_EVT_STATE_CHANGED,
    SONDE_EVT_LOCK_LOST,
} sonde_event_id_t;

ESP_EVENT_DECLARE_BASE(CFG_EVENT);

typedef enum {
    CFG_EVT_CHANGED = 0,
    CFG_EVT_FACTORY_RESET,
} cfg_event_id_t;

#define CFG_FIELD_FREQ        (1u << 0)
#define CFG_FIELD_SONDE_TYPE  (1u << 1)
#define CFG_FIELD_MYCALL      (1u << 2)
#define CFG_FIELD_BLE_ON      (1u << 3)
#define CFG_FIELD_RXBW        (1u << 4)
#define CFG_FIELD_FREQOFS     (1u << 5)
#define CFG_FIELD_BAT         (1u << 6)
#define CFG_FIELD_OLED_PERSIST (1u << 7)

#ifdef __cplusplus
}
#endif
