#include "stratos_codec.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"

#include "config_store.h"
#include "platform_common.h"
#include "battery.h"
#include "board_profile.h"

static const char *TAG = "msg";

static msg_emit_fn s_emit;
static void       *s_ctx;
static char        s_inbuf[256];
static size_t      s_inlen;

static const char *type_str(sonde_type_t t)
{
    switch (t) {
        case SONDE_TYPE_RS41:  return "RS41";
        case SONDE_TYPE_M20:   return "M20";
        case SONDE_TYPE_M10:   return "M10";
        case SONDE_TYPE_PILOT: return "PILOT";
        case SONDE_TYPE_DFM:   return "DFM";
        default:               return "RS41";
    }
}

static int sign_dbm_to_bars(int dbm)
{
    if (dbm == 0)        return -1;
    if (dbm > -70)       return 5;
    if (dbm > -85)       return 4;
    if (dbm > -95)       return 3;
    if (dbm > -105)      return 2;
    return 1;
}

static int emit(const char *fmt, ...)
{
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    if (n > (int)sizeof(buf)) n = sizeof(buf);
    if (s_emit) return s_emit(buf, n, s_ctx);
    return 0;
}

void msg_codec_init(msg_emit_fn fn, void *ctx)
{
    s_emit = fn;
    s_ctx  = ctx;
    s_inlen = 0;
}

void msg_codec_emit_state(const sonde_frame_t *f)
{
    if (!s_emit) return;
    st_config_t c = st_config_get();
    int sign = sign_dbm_to_bars(f ? f->rssi_dbm : 0);
    int bat_pct = st_battery_pct();
    int bat_mv  = st_battery_mv();
    const char *buz = "-1";
    const char *ver = st_version_string();
    float freq_mhz = c.freq_khz / 1000.0f;

    if (!f || f->state == SONDE_STATE_NO_SIGNAL) {
        emit("0/%s/%.3f/%d/%d/%d/%s/%s/o",
             type_str(c.sonde_type), freq_mhz, sign, bat_pct, bat_mv, buz, ver);
        return;
    }
    if (f->state == SONDE_STATE_TRACKING) {
        emit("1/%s/%.3f/%s/%.5f/%.5f/%ld/%.1f/%.1f/%d/%d/%ld/%d/%ld/%d/%s/0/0/0/%s/o",
             type_str(c.sonde_type), freq_mhz, f->name,
             f->lat, f->lon, (long)f->alt_m,
             f->h_vel_kmh, f->v_vel_ms,
             sign, bat_pct, (long)f->afc_hz,
             f->bk_active ? 1 : 0, (long)f->bk_remaining_s,
             bat_mv, buz, ver);
        return;
    }
    /* NAME_ONLY */
    emit("2/%s/%.3f/%s/%d/%d/%ld/%d/%s/%s/o",
         type_str(c.sonde_type), freq_mhz, f->name,
         sign, bat_pct, (long)f->afc_hz, bat_mv, buz, ver);
}

void msg_codec_emit_settings(void)
{
    if (!s_emit) return;
    st_config_t c = st_config_get();
    const board_profile_t *bp = board_profile_active();
    float freq_mhz = c.freq_khz / 1000.0f;

    /* Frame 3 per FSD §10 Appendix B / FR-14.3 — board-profile pin values. */
    emit("3/%s/%.3f/%d/%d/%d/%d/%u/%u/%u/%u/%u/%s/%ld/%d/%u/%u/%u/0/%d/-1/%s/o",
         type_str(c.sonde_type), freq_mhz,
         (int)bp->oled_sda, (int)bp->oled_scl, (int)bp->oled_rst, (int)bp->led,
         c.rxbw_idx_rs41, c.rxbw_idx_m20, c.rxbw_idx_m10,
         c.rxbw_idx_pilot, c.rxbw_idx_dfm,
         c.mycall, (long)c.freqofs_hz, (int)bp->battery_adc,
         c.vbat_min_mv, c.vbat_max_mv, c.vbat_type,
         c.aprs_name ? 1 : 0,
         st_version_string());
}

extern void st_schedule_reboot(int ms);
extern void st_factory_reset_and_reboot(int ms);

/* Parse one slash-separated key[=value] item. */
static void handle_item(const char *item)
{
    if (!*item) return;
    /* Single-word commands */
    if (!strcmp(item, "?")) {
        msg_codec_emit_settings();
        return;
    }
    if (!strcmp(item, "re")) {
        ESP_LOGI(TAG, "reboot requested");
        st_schedule_reboot(500);
        return;
    }
    if (!strcmp(item, "Re")) {
        ESP_LOGI(TAG, "factory reset + reboot requested");
        st_factory_reset_and_reboot(500);
        return;
    }
    /* key=value */
    const char *eq = strchr(item, '=');
    if (!eq) return;
    char k[24] = {0};
    size_t klen = (size_t)(eq - item);
    if (klen >= sizeof(k)) klen = sizeof(k) - 1;
    memcpy(k, item, klen);
    const char *v = eq + 1;

    st_config_t c = st_config_get();
    uint32_t mask = 0;

    if (!strcmp(k, "f")) {
        float mhz = strtof(v, NULL);
        long khz = (long)(mhz * 1000.0f + 0.5f);
        if (khz >= 137200 && khz <= 524800 && khz != (long)c.freq_khz) {
            c.freq_khz = (uint32_t)khz;
            mask |= CFG_FIELD_FREQ;
        }
    } else if (!strcmp(k, "tipo")) {
        long t = strtol(v, NULL, 10);
        if (t >= SONDE_TYPE_RS41 && t <= SONDE_TYPE_DFM && t != c.sonde_type) {
            c.sonde_type = (sonde_type_t)t;
            mask |= CFG_FIELD_SONDE_TYPE;
        }
    } else if (!strcmp(k, "myCall") || !strcmp(k, "mycall")) {
        strlcpy(c.mycall, v, sizeof(c.mycall));
        mask |= CFG_FIELD_MYCALL;
    } else if (!strcmp(k, "mute")) {
        c.mute = strtol(v, NULL, 10) != 0;
        mask |= CFG_FIELD_BLE_ON;
    } else if (!strcmp(k, "aprsName")) {
        c.aprs_name = strtol(v, NULL, 10) != 0;
    } else if (!strcmp(k, "freqofs")) {
        c.freqofs_hz = strtol(v, NULL, 10);
        mask |= CFG_FIELD_FREQOFS;
    } else if (!strcmp(k, "vBatMin")) {
        long mv = strtol(v, NULL, 10);
        if (mv >= 2000 && mv <= 5000) { c.vbat_min_mv = (uint16_t)mv; mask |= CFG_FIELD_BAT; }
    } else if (!strcmp(k, "vBatMax")) {
        long mv = strtol(v, NULL, 10);
        if (mv >= 2000 && mv <= 5000) { c.vbat_max_mv = (uint16_t)mv; mask |= CFG_FIELD_BAT; }
    } else if (!strcmp(k, "vBatType")) {
        long ty = strtol(v, NULL, 10);
        if (ty >= 0 && ty <= 2) { c.vbat_type = (uint8_t)ty; mask |= CFG_FIELD_BAT; }
    } else if (!strcmp(k, "rs41.rxbw")) {
        long ix = strtol(v, NULL, 10);
        if (ix >= 0 && ix < 32) { c.rxbw_idx_rs41 = (uint8_t)ix; mask |= CFG_FIELD_RXBW; }
    } else if (!strcmp(k, "m20.rxbw")) {
        c.rxbw_idx_m20 = (uint8_t)strtol(v, NULL, 10); mask |= CFG_FIELD_RXBW;
    } else if (!strcmp(k, "m10.rxbw")) {
        c.rxbw_idx_m10 = (uint8_t)strtol(v, NULL, 10); mask |= CFG_FIELD_RXBW;
    } else if (!strcmp(k, "pilot.rxbw")) {
        c.rxbw_idx_pilot = (uint8_t)strtol(v, NULL, 10); mask |= CFG_FIELD_RXBW;
    } else if (!strcmp(k, "dfm.rxbw")) {
        c.rxbw_idx_dfm = (uint8_t)strtol(v, NULL, 10); mask |= CFG_FIELD_RXBW;
    } else if (!strcmp(k, "sleep")) {
        ESP_LOGI(TAG, "sleep=%s — no-op (FR-5.10)", v);
    } else {
        /* Pin-config + serial-config — accept-and-ignore (FR-5.9). */
        ESP_LOGD(TAG, "ignored: %s=%s", k, v);
    }

    if (mask) st_config_save(&c, mask);
}

esp_err_t msg_codec_handle_input(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        if (s_inlen < sizeof(s_inbuf) - 1) s_inbuf[s_inlen++] = c;
        else s_inlen = 0;       /* overflow → drop */
    }
    /* Look for o{...}o */
    char *start = strstr(s_inbuf, "o{");
    while (start) {
        char *end = strstr(start + 2, "}o");
        if (!end) break;
        *end = 0;
        char *body = start + 2;
        char *tok = strtok(body, "/");
        while (tok) {
            handle_item(tok);
            tok = strtok(NULL, "/");
        }
        size_t consumed = (size_t)(end - s_inbuf) + 2;
        memmove(s_inbuf, s_inbuf + consumed, s_inlen - consumed);
        s_inlen -= consumed;
        s_inbuf[s_inlen] = 0;
        start = strstr(s_inbuf, "o{");
    }
    return ESP_OK;
}
