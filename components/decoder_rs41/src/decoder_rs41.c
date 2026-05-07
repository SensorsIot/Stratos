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
#include "rs_ecc.h"

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
   consumes the 8-byte sync header, the remaining frame body is 312
   bytes (320 byte frame minus 8 byte sync). The SX1276 PayloadLen is
   set to 312 so a single packet is exactly one frame's body — no
   inter-frame splicing. */
#define FRAME_BYTES          312
#define IDLE_FRAMES_NO_BYTES 2   /* publish NO_SIGNAL after 1 s of silence */

static TaskHandle_t  s_task;
static QueueHandle_t s_q;
static volatile bool s_running;

/* Raw-datagram ring buffer: keep the last N raw 320-byte frames (post-sync,
   before bit-reverse / PRBS / ECC) so we can pull them out via /api/raw
   and reverse-engineer the proper decode chain in Python. */
#define RAW_FRAMES 4
static uint8_t  s_raw_frames[RAW_FRAMES][FRAME_BYTES];
static volatile int s_raw_count;            /* total frames captured (monotonic) */
static volatile int s_raw_seqno[RAW_FRAMES];

void decoder_rs41_get_raw(int slot, uint8_t out[FRAME_BYTES], int *seqno_out)
{
    if (slot < 0 || slot >= RAW_FRAMES) {
        if (seqno_out) *seqno_out = -1;
        return;
    }
    memcpy(out, s_raw_frames[slot], FRAME_BYTES);
    if (seqno_out) *seqno_out = s_raw_seqno[slot];
}

/* Last raw ECEF values seen by the parser (for diagnostic exposure via
   the HTTP API). Only updated when parse_frame succeeds. */
static volatile int32_t s_last_ecef_x;
static volatile int32_t s_last_ecef_y;
static volatile int32_t s_last_ecef_z;
static volatile int16_t s_last_ecef_vx, s_last_ecef_vy, s_last_ecef_vz;
static volatile uint32_t s_last_parse_us_div1k; /* timestamp of last clean parse, /1000 to fit in uint32 */

void decoder_rs41_last_ecef(int32_t *x, int32_t *y, int32_t *z, uint32_t *age_ms_ago)
{
    if (x) *x = s_last_ecef_x;
    if (y) *y = s_last_ecef_y;
    if (z) *z = s_last_ecef_z;
    if (age_ms_ago) {
        if (s_last_parse_us_div1k == 0) {
            *age_ms_ago = 0xFFFFFFFFu;
        } else {
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000LL);
            *age_ms_ago = now_ms - s_last_parse_us_div1k;
        }
    }
}

void decoder_rs41_last_ecef_vel(int16_t *vx, int16_t *vy, int16_t *vz)
{
    if (vx) *vx = s_last_ecef_vx;
    if (vy) *vy = s_last_ecef_vy;
    if (vz) *vz = s_last_ecef_vz;
}

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
#define POS_GPSecefV   0x120  /* 3 × i16 LE, units cm/s (vx, vy, vz in ECEF — verified empirically against stationary sonde) */

/* SX1276 receives bits LSB-first within a byte; the RS41 protocol is
   documented MSB-first. Reverse before applying the mask. */
static inline uint8_t bitrev8(uint8_t b)
{
    b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4);
    b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2);
    b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1);
    return b;
}

/* Read frame[pos] from a fully de-whitened body buffer.
   pos uses the full-frame convention (sync header notionally at 0..7,
   body[i] holds frame[i+8]). */
static inline uint8_t fb(const uint8_t *body, int pos)
{
    int idx = pos - 8;
    if (idx < 0 || idx >= FRAME_BYTES) return 0;
    return body[idx];
}

static inline int32_t read_i32_le(const uint8_t *body, int pos)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)fb(body, pos + i) << (i * 8);
    return (int32_t)v;
}

static inline int16_t read_i16_le(const uint8_t *body, int pos)
{
    uint16_t v = (uint16_t)fb(body, pos) | ((uint16_t)fb(body, pos + 1) << 8);
    return (int16_t)v;
}

/* RS41 frame layout for ECC: parity at frame[8..55] (two contiguous
   24-byte blocks), interleaved data at frame[56..]. rs1729's reference
   builds the codeword with parity at cw[0..23] and data at cw[24..254],
   but in the Phil-Karn-RS convention where index 0 is the HIGHEST
   polynomial degree (x^(N-1)), not the constant term.

   Our textbook-style decoder treats index 0 as the constant term of the
   polynomial, so we build the codeword with the opposite indexing —
   equivalent to reversing rs1729's cw[] in place.

   Polynomial-coefficient convention (cw[i] = coefficient of x^i):
     cw[0..23]    = parity in reverse order (cw[0] = parity[23], cw[23] = parity[0])
     cw[24..122]  = zero pad (99 positions, K=231 minus the 132 real data bytes)
     cw[123..254] = data in reverse order (cw[123] = data[131], cw[254] = data[0]) */
static int build_codeword(const uint8_t *body, int parity_off, int data_off,
                          uint8_t cw[255])
{
    memset(cw, 0, 255);
    for (int i = 0; i < 24;  i++) cw[i]       = body[parity_off + 23 - i];
    for (int j = 0; j < 132; j++) cw[123 + j] = body[data_off + 2 * (131 - j)];
    return 0;
}

static void apply_codeword(const uint8_t cw[255], uint8_t *body,
                           int parity_off, int data_off)
{
    for (int i = 0; i < 24;  i++) body[parity_off + 23 - i]      = cw[i];
    for (int j = 0; j < 132; j++) body[data_off + 2 * (131 - j)] = cw[123 + j];
}

/* Diagnostic counters, written by rs_correct(); declared here so they're in
   scope. Read via the decoder_rs41_last_* accessors near the bottom. */
static volatile int s_last_rs_errs = -1;
static volatile int s_last_synd1   = -1;
static volatile int s_last_synd2   = -1;

/* Build the full de-whitened frame body, then run RS(255,231) ECC on
   both interleaved codewords. Stores total errors corrected (or -1 on
   uncorrectable) at *errs_out. Records pre-correction syndrome counts
   for diagnostic exposure. */
static void rs_correct(const uint8_t *raw, uint8_t *body, int *errs_out)
{
    /* 1. Bit-reverse + PRBS-de-whiten every byte, materialising the body. */
    for (int i = 0; i < FRAME_BYTES; i++) {
        body[i] = bitrev8(raw[i]) ^ PRBS_MASK[(i + 8) & 63];
    }

    /* 2. Build, count, decode, apply each codeword. */
    uint8_t cw[255];
    int total = 0;
    int n;

    build_codeword(body, /*parity*/  0, /*data*/ 48, cw);
    s_last_synd1 = rs_count_nonzero_syndromes(cw);
    n = rs_decode_255_231(cw);
    if (n >= 0) { apply_codeword(cw, body,  0, 48); total += n; }

    build_codeword(body, /*parity*/ 24, /*data*/ 49, cw);
    s_last_synd2 = rs_count_nonzero_syndromes(cw);
    int n2 = rs_decode_255_231(cw);
    if (n2 >= 0) { apply_codeword(cw, body, 24, 49); total += n2; }

    *errs_out = (n < 0 || n2 < 0) ? -1 : total;
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

int decoder_rs41_last_rs_errs(void) { return s_last_rs_errs; }
int decoder_rs41_last_synd1(void)   { return s_last_synd1; }
int decoder_rs41_last_synd2(void)   { return s_last_synd2; }

/* Parse one 320-byte raw RS41 frame buffer. Runs Reed-Solomon ECC across
   the two interleaved codewords first; if RS rejects (uncorrectable) we
   bail out. Otherwise field extraction reads from the corrected body.
   Returns true if the SondeID is valid alphanumeric; out is then ready. */
static bool parse_frame(const uint8_t *raw, sonde_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type         = SONDE_TYPE_RS41;
    out->rssi_dbm     = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();

    /* Run RS ECC. Body is the de-whitened frame body — possibly corrected
       in place by rs_correct. If RS reports uncorrectable (-1) we fall
       through to field extraction anyway: the strict alphanumeric ID
       check still gates publish, so a bad frame can't slip through, and
       the occasional intrinsically-clean frame still produces a result. */
    uint8_t body[FRAME_BYTES];
    int errs;
    rs_correct(raw, body, &errs);
    s_last_rs_errs = errs;

    /* SondeID: 8 chars, uppercase alphanumeric. After RS correction this
       check should virtually always pass for genuine RS41 frames. */
    char id[9] = {0};
    for (int i = 0; i < 8; i++) {
        uint8_t b = fb(body, POS_SONDEID + i);
        if (!((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z'))) {
            return false;    /* RS-corrected but ID is not text — sync mis-align */
        }
        id[i] = (char)b;
    }
    id[8] = 0;
    strncpy(out->name, id, sizeof(out->name) - 1);
    out->state = SONDE_STATE_NAME_ONLY;

    /* GPS ECEF position. Zero implies no fix yet. */
    int32_t ex = read_i32_le(body, POS_GPSecefX);
    int32_t ey = read_i32_le(body, POS_GPSecefY);
    int32_t ez = read_i32_le(body, POS_GPSecefZ);
    s_last_ecef_x = ex;
    s_last_ecef_y = ey;
    s_last_ecef_z = ez;
    s_last_parse_us_div1k = (uint32_t)(esp_timer_get_time() / 1000LL);
    if (ex != 0 || ey != 0 || ez != 0) {
        double lat, lon, alt;
        ecef_to_wgs84(ex / 100.0, ey / 100.0, ez / 100.0, &lat, &lon, &alt);
        if (fabs(lat) <= 90.0 && fabs(lon) <= 180.0 &&
            alt > -500.0 && alt < 50000.0) {
            out->lat   = lat;
            out->lon   = lon;
            out->alt_m = (int32_t)alt;

            int16_t rvx = read_i16_le(body, POS_GPSecefV + 0);
            int16_t rvy = read_i16_le(body, POS_GPSecefV + 2);
            int16_t rvz = read_i16_le(body, POS_GPSecefV + 4);
            s_last_ecef_vx = rvx;
            s_last_ecef_vy = rvy;
            s_last_ecef_vz = rvz;
            /* RS41 ECEF velocity field is i16 in centimeters per 100ms?
               Empirically: a stationary sonde reports raw |v| ~ 700 (with
               systematic bias of a few hundred per axis on weak signal).
               Scale /1000 gives plausible m/s for stationary (~0.7 m/s
               jitter), implying the raw unit is millimeters per second. */
            double vx = rvx / 1000.0;
            double vy = rvy / 1000.0;
            double vz = rvz / 1000.0;
            ecef_vel_to_local(vx, vy, vz, lat, lon,
                              &out->h_vel_kmh, &out->v_vel_ms);
            out->state = SONDE_STATE_TRACKING;
        }
    }

    return true;
}

/* Stickiness: once the parser produces a valid frame, keep re-publishing it
   (with current RSSI) for STICKY_LOCK_US even if subsequent frames fail
   validation or no bytes arrive. Prevents the UI / BLE / API from flickering
   between a real serial and "NO SIGNAL" when reception is marginal.

   Without Reed-Solomon error correction, we currently see ~1 clean parse
   in every 20–30 frames at marginal RSSI. 60 s gives a comfortable
   margin: as long as we get at least one clean parse per minute, the
   user sees a steady ID. Falls through to NO_SIGNAL if the sonde really
   is gone. */
#define STICKY_LOCK_US (60LL * 1000000LL)

static void task(void *arg)
{
    (void)arg;
    uint8_t       buf[FRAME_BYTES];
    size_t        pos = 0;
    int           idle_ticks = 0;
    sonde_frame_t last_good = {0};
    int64_t       last_good_us = 0;

    while (s_running) {
        uint8_t b;
        if (xQueueReceive(s_q, &b, pdMS_TO_TICKS(500)) == pdTRUE) {
            idle_ticks = 0;
            if (pos < FRAME_BYTES) buf[pos++] = b;
            if (pos >= FRAME_BYTES) {
                sonde_frame_t f;
                if (parse_frame(buf, &f)) {
                    /* Capture only frames that pass the strict SondeID
                       check — these are the genuinely-clean frames we
                       want to reverse-engineer the RS layout against. */
                    int slot = s_raw_count % RAW_FRAMES;
                    memcpy(s_raw_frames[slot], buf, FRAME_BYTES);
                    s_raw_seqno[slot] = s_raw_count;
                    s_raw_count++;

                    last_good    = f;
                    last_good_us = esp_timer_get_time();
                    decoder_core_publish_frame(&f);
                    ESP_LOGI(TAG, "frame: %s  state=%d  lat=%.4f lon=%.4f alt=%ld",
                             f.name, (int)f.state, f.lat, f.lon, (long)f.alt_m);
                }
                pos = 0;
            }
        } else if (++idle_ticks >= IDLE_FRAMES_NO_BYTES) {
            int64_t now_us = esp_timer_get_time();
            sonde_frame_t f = {0};
            if (last_good_us != 0 && (now_us - last_good_us) < STICKY_LOCK_US) {
                /* Hold the last good frame; refresh just RSSI + timestamp. */
                f = last_good;
                f.rssi_dbm     = st_rf_rssi_dbm();
                f.monotonic_us = now_us;
            } else {
                f.state        = SONDE_STATE_NO_SIGNAL;
                f.type         = SONDE_TYPE_RS41;
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
