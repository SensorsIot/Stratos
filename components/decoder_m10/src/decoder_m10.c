/*
 * decoder_m10 — Meteomodem M10 frame consumer (skeleton).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SensorsIot
 *
 * Original implementation. Protocol facts (RF parameters, frame layout,
 * scrambling/CRC scheme) are publicly documented; rs1729/RS and
 * dl9rdz/rdz_ttgo_sonde are the canonical references. No code copied.
 *
 * STATUS: skeleton — the parser publishes a synthetic NAME_ONLY frame
 * when bytes arrive on the queue, to validate the decoder_core plumbing
 * and runtime decoder switching. Real M10 frame parsing (CRC validation,
 * GPS / PTU extraction) lands in follow-up commits.
 *
 * RF profile (set in rf_sx1276.c s_profiles[]): GFSK, 9600 bps,
 * fdev ±2.4 kHz, RxBw 25 kHz, sync 0x66 (1 byte) — per
 * dl9rdz/rdz_ttgo_sonde M10M20.cpp known-working values.
 *
 * Frame layout (from public references):
 *   length      = 101 bytes
 *   CRC at pos  = 99 (16-bit, custom rolling, see crc_M10 in references)
 *   payload     = bytes 0..98 (mostly raw bytes; certain fields use
 *                 bit-twiddled big-endian integers)
 *
 * Key field offsets (TBD against captured frames):
 *   serial      ~ ?
 *   GPS lat/lon = i32 BE at ?
 *   GPS alt     = i24 BE at ?
 *   GPS week    = u16 BE at ?
 *   ECEF vel    = 3 × i16 BE at ?
 */

#include "decoder_m10.h"
#include "rf_sx1276.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sonde_types.h"

static const char *TAG = "m10";

#define FRAME_BYTES          101
#define IDLE_FRAMES_NO_BYTES 2

static TaskHandle_t  s_task;
static QueueHandle_t s_q;
static volatile bool s_running;

/* Parse one raw M10 frame. Returns true if payload validates (CRC OK,
   plausible serial). For the skeleton, accept any non-empty buffer and
   emit a NAME_ONLY placeholder. */
static bool parse_frame(const uint8_t *raw, sonde_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type         = SONDE_TYPE_M10;
    out->rssi_dbm     = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();
    out->state        = SONDE_STATE_NAME_ONLY;
    /* TODO: real CRC + field extraction. Until then, emit a placeholder
       name so upper layers exercise their full pipeline. */
    snprintf(out->name, sizeof(out->name), "M10-PRE");
    (void)raw;
    return true;
}

static void task(void *arg)
{
    (void)arg;
    uint8_t buf[FRAME_BYTES];
    size_t  pos = 0;
    int     idle_ticks = 0;

    while (s_running) {
        uint8_t b;
        if (xQueueReceive(s_q, &b, pdMS_TO_TICKS(500)) == pdTRUE) {
            idle_ticks = 0;
            if (pos < FRAME_BYTES) buf[pos++] = b;
            if (pos >= FRAME_BYTES) {
                sonde_frame_t f;
                if (parse_frame(buf, &f)) {
                    decoder_core_publish_frame(&f);
                    ESP_LOGI(TAG, "frame: %s state=%d", f.name, (int)f.state);
                }
                pos = 0;
            }
        } else if (++idle_ticks >= IDLE_FRAMES_NO_BYTES) {
            sonde_frame_t f = {0};
            f.state        = SONDE_STATE_NO_SIGNAL;
            f.type         = SONDE_TYPE_M10;
            f.rssi_dbm     = st_rf_rssi_dbm();
            f.monotonic_us = esp_timer_get_time();
            decoder_core_publish_frame(&f);
            idle_ticks = 0;
            pos = 0;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t m10_start(QueueHandle_t byte_q)
{
    if (s_running) return ESP_OK;
    s_q       = byte_q;
    s_running = true;

    const rf_profile_t *p = st_rf_profile_for(SONDE_TYPE_M10);
    if (p) st_rf_apply_profile(p);

    BaseType_t r = xTaskCreate(task, "m10", 8192, NULL, 10, &s_task);
    if (r != pdPASS) { s_running = false; return ESP_FAIL; }
    ESP_LOGI(TAG, "decoder started");
    return ESP_OK;
}

static esp_err_t m10_stop(void)
{
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static const decoder_vtable_t s_vtable = {
    .type  = SONDE_TYPE_M10,
    .name  = "m10",
    .start = m10_start,
    .stop  = m10_stop,
};

const decoder_vtable_t *decoder_m10_vtable(void) { return &s_vtable; }
