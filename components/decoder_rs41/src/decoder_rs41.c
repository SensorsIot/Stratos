/*
 * decoder_rs41 — Vaisala RS41 frame consumer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SensorsIot
 *
 * Original implementation. The RS41 protocol facts used here (PRBS
 * whitening sequence, subframe layout, ECEF→WGS84 conversion, bit-order
 * convention) are publicly documented properties of the Vaisala RS41
 * hardware. No code is copied from any third-party project.
 *
 * Pipeline:
 *   SX1276 sync match (8-byte header) → FIFO bytes (LSB-first chip-rate)
 *     → bit-reverse + XOR-de-whiten with the 64-byte PRBS mask
 *     → field extraction at fixed offsets (SondeID, ECEF position, ECEF
 *       velocity)
 *     → ECEF (cm/s) → WGS84 lat/lon/alt + horizontal/vertical speed
 *     → publish sonde_frame_t to the SONDE_EVENT loop.
 *
 * No Reed-Solomon error correction yet — frames with bit errors get
 * dropped via the SondeID printable-ASCII sanity check. RS error
 * correction is a future enhancement.
 */

#include "decoder_rs41.h"
#include "rf_sx1276.h"

#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sonde_types.h"

static const char *TAG = "rs41";

/* RS41 transmits one frame per second. After the SX1276 sync match
   consumes the 8-byte sync header, the remaining frame body is 320
   bytes (chip-rate, post-sync). */
#define FRAME_BYTES          320
#define IDLE_FRAMES_NO_BYTES 2   /* publish NO_SIGNAL after 1 s of silence */

static TaskHandle_t  s_task;
static QueueHandle_t s_q;
static volatile bool s_running;

/* RS41 PRBS whitening sequence — a hardware-defined constant of the
   transmit chain. Values per the publicly-documented RS41 protocol. */
static const uint8_t PRBS_MASK[64] = {
    0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98,
    0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
    0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1,
    0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
    0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C,
    0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
    0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23,
    0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1,
};

/* Frame field offsets — positions are in the full RS41 frame including
   the 8-byte sync header at offset 0..7. Our raw buffer starts at
   offset 8 (the sync detector consumed the header). */
#define POS_FRAMENB    0x3B   /* u16 LE (frame number) */
#define POS_SONDEID    0x3D   /* 8 ASCII bytes (serial number) */
#define POS_GPSecefX   0x114  /* i32 LE, units cm */
#define POS_GPSecefY   0x118  /* i32 LE, units cm */
#define POS_GPSecefZ   0x11C  /* i32 LE, units cm */
#define POS_GPSecefV   0x120  /* 3 × i16 LE, units cm/s (vx, vy, vz in ECEF) */

/* SX1276 receives bits LSB-first within a byte; the RS41 protocol is
   documented MSB-first. Reverse before applying the mask. */
static inline uint8_t bitrev8(uint8_t b)
{
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

/* Read frame[pos] from the post-sync raw buffer.
   pos uses the full-frame convention (sync at 0..7, body at 8..). */
static inline uint8_t fb(const uint8_t *raw, int pos)
{
    int idx = pos - 8;
    if (idx < 0 || idx >= FRAME_BYTES) return 0;
    return bitrev8(raw[idx]) ^ PRBS_MASK[pos & 63];
}

static inline int32_t read_i32_le(const uint8_t *raw, int pos)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)fb(raw, pos + i) << (i * 8);
    return (int32_t)v;
}

static inline int16_t read_i16_le(const uint8_t *raw, int pos)
{
    uint16_t v = (uint16_t)fb(raw, pos) | ((uint16_t)fb(raw, pos + 1) << 8);
    return (int16_t)v;
}

/* ECEF (meters) → WGS84 geodetic (degrees, meters). Iterative; converges
   in ≤ 4 steps for terrestrial radii. */
static void ecef_to_wgs84(double x, double y, double z,
                          double *lat, double *lon, double *alt)
{
    static const double a  = 6378137.0;            /* WGS84 semi-major axis */
    static const double e2 = 6.69437999014e-3;     /* first eccentricity² */
    double p = sqrt(x * x + y * y);
    *lon = atan2(y, x) * 180.0 / M_PI;
    double phi = atan2(z, p * (1.0 - e2));
    double N = a;
    for (int i = 0; i < 5; i++) {
        double s = sin(phi);
        N = a / sqrt(1.0 - e2 * s * s);
        phi = atan2(z + e2 * N * s, p);
    }
    *lat = phi * 180.0 / M_PI;
    *alt = p / cos(phi) - N;
}

/* ECEF velocity (m/s) → horizontal speed (km/h) and vertical (m/s, +up)
   in the local tangent plane at the given lat/lon. */
static void ecef_vel_to_local(double vx, double vy, double vz,
                              double lat_deg, double lon_deg,
                              float *h_kmh, float *v_ms)
{
    double lat = lat_deg * M_PI / 180.0;
    double lon = lon_deg * M_PI / 180.0;
    double sl = sin(lat), cl = cos(lat);
    double so = sin(lon), co = cos(lon);
    /* Up component: project ECEF velocity onto the up unit vector. */
    double vu = cl * co * vx + cl * so * vy + sl * vz;
    /* East: -sin(lon)·vx + cos(lon)·vy */
    double ve = -so * vx + co * vy;
    /* North: -sin(lat)·cos(lon)·vx - sin(lat)·sin(lon)·vy + cos(lat)·vz */
    double vn = -sl * co * vx - sl * so * vy + cl * vz;
    *h_kmh = (float)(sqrt(ve * ve + vn * vn) * 3.6);
    *v_ms  = (float)vu;
}

/* Parse one 320-byte raw RS41 frame buffer. Returns true if SondeID
   passes a printable-ASCII sanity check; the caller can assume *out is
   ready to publish in that case. */
static bool parse_frame(const uint8_t *raw, sonde_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type         = SONDE_TYPE_RS41;
    out->rssi_dbm     = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();

    /* SondeID (8 ASCII bytes, alphanumeric in the wild). */
    int valid_chars = 0;
    char id[9] = {0};
    for (int i = 0; i < 8; i++) {
        uint8_t b = fb(raw, POS_SONDEID + i);
        if ((b >= '0' && b <= '9') ||
            (b >= 'A' && b <= 'Z') ||
            (b >= 'a' && b <= 'z')) {
            id[i] = (char)b;
            valid_chars++;
        } else {
            id[i] = '?';
        }
    }
    id[8] = 0;
    if (valid_chars < 6) {
        /* Junk frame (sync misalignment or uncorrectable bit errors). */
        return false;
    }
    strncpy(out->name, id, sizeof(out->name) - 1);
    out->state = SONDE_STATE_NAME_ONLY;

    /* GPS ECEF position. Zero implies no fix yet. */
    int32_t ex = read_i32_le(raw, POS_GPSecefX);
    int32_t ey = read_i32_le(raw, POS_GPSecefY);
    int32_t ez = read_i32_le(raw, POS_GPSecefZ);
    if (ex != 0 || ey != 0 || ez != 0) {
        double lat, lon, alt;
        ecef_to_wgs84(ex / 100.0, ey / 100.0, ez / 100.0, &lat, &lon, &alt);
        if (fabs(lat) <= 90.0 && fabs(lon) <= 180.0 &&
            alt > -500.0 && alt < 50000.0) {
            out->lat   = lat;
            out->lon   = lon;
            out->alt_m = (int32_t)alt;

            double vx = read_i16_le(raw, POS_GPSecefV + 0) / 100.0;
            double vy = read_i16_le(raw, POS_GPSecefV + 2) / 100.0;
            double vz = read_i16_le(raw, POS_GPSecefV + 4) / 100.0;
            ecef_vel_to_local(vx, vy, vz, lat, lon,
                              &out->h_vel_kmh, &out->v_vel_ms);
            out->state = SONDE_STATE_TRACKING;
        }
    }

    return true;
}

static void task(void *arg)
{
    (void)arg;
    uint8_t  buf[FRAME_BYTES];
    size_t   pos = 0;
    int      idle_ticks = 0;

    while (s_running) {
        uint8_t b;
        if (xQueueReceive(s_q, &b, pdMS_TO_TICKS(500)) == pdTRUE) {
            idle_ticks = 0;
            if (pos < FRAME_BYTES) buf[pos++] = b;
            if (pos >= FRAME_BYTES) {
                sonde_frame_t f;
                if (parse_frame(buf, &f)) {
                    decoder_core_publish_frame(&f);
                    ESP_LOGI(TAG, "frame: %s  state=%d  lat=%.4f lon=%.4f alt=%ld",
                             f.name, (int)f.state, f.lat, f.lon, (long)f.alt_m);
                }
                pos = 0;
            }
        } else {
            if (++idle_ticks >= IDLE_FRAMES_NO_BYTES) {
                sonde_frame_t f = {0};
                f.state        = SONDE_STATE_NO_SIGNAL;
                f.type         = SONDE_TYPE_RS41;
                f.rssi_dbm     = st_rf_rssi_dbm();
                f.monotonic_us = esp_timer_get_time();
                decoder_core_publish_frame(&f);
                idle_ticks = 0;
                pos = 0;
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t rs41_start(QueueHandle_t byte_q)
{
    if (s_running) return ESP_OK;
    s_q       = byte_q;
    s_running = true;

    const rf_profile_t *p = st_rf_profile_for(SONDE_TYPE_RS41);
    if (p) st_rf_apply_profile(p);

    BaseType_t r = xTaskCreate(task, "rs41", 8192, NULL, 10, &s_task);
    if (r != pdPASS) { s_running = false; return ESP_FAIL; }
    ESP_LOGI(TAG, "decoder started");
    return ESP_OK;
}

static esp_err_t rs41_stop(void)
{
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static const decoder_vtable_t s_vtable = {
    .type  = SONDE_TYPE_RS41,
    .name  = "rs41",
    .start = rs41_start,
    .stop  = rs41_stop,
};

const decoder_vtable_t *decoder_rs41_vtable(void) { return &s_vtable; }
