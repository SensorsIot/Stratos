/*
 * decoder_m20 — Meteomodem M20 frame consumer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SensorsIot
 *
 * Same architecture as decoder_m10 (Manchester chip → data, custom
 * rolling CRC, fixed-offset field extraction). Only the frame length,
 * field offsets, and scale factors differ.
 *
 * Frame layout (data bytes, ~88 bytes, BE throughout):
 *    8  i24 BE   altitude     × 0.01  m
 *   11  i16 BE   v_east       × 0.01  m/s
 *   13  i16 BE   v_north      × 0.01  m/s
 *   24  i16 BE   v_up         × 0.01  m/s
 *   28  i32 BE   latitude     × 1e-6  deg
 *   32  i32 BE   longitude    × 1e-6  deg
 *   86  u16 BE   CRC          covers bytes 0..85
 */

#include "decoder_m20.h"
#include "rf_sx1276.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sonde_types.h"

static const char *TAG = "m20";

#define CHIP_BYTES           202
#define DATA_BYTES           101   /* same chip-byte budget as M10 to match radio profile */
#define M20_FRAMELEN         88
#define M20_CRC_POS          86
#define IDLE_FRAMES_NO_BYTES 2

#define VMUL_M20  0.01f

static TaskHandle_t  s_task;
static QueueHandle_t s_q;
static volatile bool s_running;

static inline uint8_t bitrev8(uint8_t b)
{
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

static void manchester_decode(const uint8_t *chips, uint8_t *out)
{
    int out_bit = 0;
    for (int byte = 0; byte < CHIP_BYTES; byte++) {
        uint8_t b = bitrev8(chips[byte]);
        for (int bit = 7; bit >= 0; bit--) {
            int chip = (b >> bit) & 1;
            int phase = out_bit & 1;
            int data_idx = out_bit >> 1;
            int data_byte = data_idx >> 3;
            int data_bit  = 7 - (data_idx & 7);
            if (data_byte >= DATA_BYTES) goto done;
            if (phase == 0) {
                if (chip) out[data_byte] |=  (1 << data_bit);
                else      out[data_byte] &= ~(1 << data_bit);
            }
            out_bit++;
        }
    }
done:
    return;
}

static uint16_t crc_step(uint16_t c, uint8_t b)
{
    uint8_t c1 = (c >> 0) & 0xFF;
    uint8_t bb = (b >> 1) | ((b & 1) << 7);
    bb ^= (bb >> 2) & 0xFF;
    uint8_t t6 = ((c >> 0) & 1) ^ ((c >> 2) & 1) ^ ((c >> 4) & 1);
    uint8_t t7 = ((c >> 1) & 1) ^ ((c >> 3) & 1) ^ ((c >> 5) & 1);
    uint8_t t  = (c & 0x3F) | (t6 << 6) | (t7 << 7);
    uint8_t s  = (c >> 7) & 0xFF;
    s ^= (s >> 2) & 0xFF;
    uint8_t c0 = bb ^ t ^ s;
    return (uint16_t)((c1 << 8) | c0);
}

static bool m20_crc_ok(const uint8_t *frame)
{
    uint16_t cs = 0;
    for (int i = 0; i < M20_CRC_POS; i++) cs = crc_step(cs, frame[i]);
    uint16_t got = ((uint16_t)frame[M20_CRC_POS] << 8) | frame[M20_CRC_POS + 1];
    return cs == got;
}

static inline int32_t rd_i32_be(const uint8_t *p)
{
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] <<  8) |  (uint32_t)p[3]);
}
static inline int16_t rd_i16_be(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline int32_t rd_i24_be(const uint8_t *p)
{
    int32_t v = ((int32_t)p[0] << 16) | ((int32_t)p[1] << 8) | p[2];
    if (v & 0x00800000) v |= 0xFF000000; /* sign-extend */
    return v;
}

static bool parse_frame(const uint8_t *chips, sonde_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type         = SONDE_TYPE_M20;
    out->rssi_dbm     = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();

    uint8_t frame[DATA_BYTES] = {0};
    manchester_decode(chips, frame);

    if (!m20_crc_ok(frame)) {
        return false;
    }

    /* Serial number bytes are encoded across positions 19..23 in the
       reference. We render an 8-character placeholder pending a real
       decode (FSD §11.6 Phase B). */
    snprintf(out->name, sizeof(out->name), "M20-%02X%02X%02X",
             frame[19], frame[20], frame[21]);
    out->state = SONDE_STATE_NAME_ONLY;

    int32_t lat_raw = rd_i32_be(frame + 28);
    int32_t lon_raw = rd_i32_be(frame + 32);
    int32_t alt_raw = rd_i24_be(frame + 8);
    if (lat_raw != 0 || lon_raw != 0) {
        double lat = lat_raw * 1e-6;
        double lon = lon_raw * 1e-6;
        double alt = alt_raw * 0.01;
        if (fabs(lat) <= 90.0 && fabs(lon) <= 180.0 &&
            alt > -500.0 && alt < 50000.0) {
            out->lat   = lat;
            out->lon   = lon;
            out->alt_m = (int32_t)alt;

            float ve = rd_i16_be(frame + 11) * VMUL_M20;
            float vn = rd_i16_be(frame + 13) * VMUL_M20;
            float vv = rd_i16_be(frame + 24) * VMUL_M20;
            out->h_vel_kmh = sqrtf(ve * ve + vn * vn) * 3.6f;
            out->v_vel_ms  = vv;
            out->state     = SONDE_STATE_TRACKING;
        }
    }

    return true;
}

#define STICKY_LOCK_US (60LL * 1000000LL)

static void task(void *arg)
{
    (void)arg;
    uint8_t buf[CHIP_BYTES];
    size_t  pos = 0;
    int     idle_ticks = 0;
    sonde_frame_t last_good = {0};
    int64_t last_good_us = 0;

    while (s_running) {
        uint8_t b;
        if (xQueueReceive(s_q, &b, pdMS_TO_TICKS(500)) == pdTRUE) {
            idle_ticks = 0;
            if (pos < CHIP_BYTES) buf[pos++] = b;
            if (pos >= CHIP_BYTES) {
                sonde_frame_t f;
                if (parse_frame(buf, &f)) {
                    last_good = f;
                    last_good_us = esp_timer_get_time();
                    decoder_core_publish_frame(&f);
                    ESP_LOGI(TAG, "frame: %s state=%d lat=%.4f lon=%.4f",
                             f.name, (int)f.state, f.lat, f.lon);
                }
                pos = 0;
            }
        } else if (++idle_ticks >= IDLE_FRAMES_NO_BYTES) {
            int64_t now_us = esp_timer_get_time();
            sonde_frame_t f = {0};
            if (last_good_us != 0 && (now_us - last_good_us) < STICKY_LOCK_US) {
                f = last_good;
                f.rssi_dbm     = st_rf_rssi_dbm();
                f.monotonic_us = now_us;
            } else {
                f.state        = SONDE_STATE_NO_SIGNAL;
                f.type         = SONDE_TYPE_M20;
                f.rssi_dbm     = st_rf_rssi_dbm();
                f.monotonic_us = now_us;
            }
            decoder_core_publish_frame(&f);
            idle_ticks = 0;
            pos = 0;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t m20_start(QueueHandle_t byte_q)
{
    if (s_running) return ESP_OK;
    s_q       = byte_q;
    s_running = true;

    const rf_profile_t *p = st_rf_profile_for(SONDE_TYPE_M20);
    if (p) st_rf_apply_profile(p);

    BaseType_t r = xTaskCreate(task, "m20", 8192, NULL, 10, &s_task);
    if (r != pdPASS) { s_running = false; return ESP_FAIL; }
    ESP_LOGI(TAG, "decoder started");
    return ESP_OK;
}

static esp_err_t m20_stop(void)
{
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static const decoder_vtable_t s_vtable = {
    .type  = SONDE_TYPE_M20,
    .name  = "m20",
    .start = m20_start,
    .stop  = m20_stop,
};

const decoder_vtable_t *decoder_m20_vtable(void) { return &s_vtable; }
