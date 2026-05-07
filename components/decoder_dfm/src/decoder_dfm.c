/*
 * decoder_dfm — Graw DFM-09 / DFM-17 frame consumer (skeleton).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SensorsIot
 *
 * Per FSD §11.6 Phase B, full DFM decode (multi-subframe interleaver,
 * Hamming(8,4) FEC, BCH error correction, configuration-frame
 * subframe-id reassembly, GPS subframe extraction) is staged after M10
 * and M20 are flying.
 *
 * This skeleton:
 *   - Registers a decoder_vtable_t so the radio profile can be picked
 *     by the user and the decoder activated.
 *   - Drains chip-rate bytes from the FIFO queue.
 *   - Manchester-decodes and publishes a NAME_ONLY placeholder frame
 *     whenever the chip-byte buffer fills, so the user can verify the
 *     RF chain is locking onto a DFM signal even before parsing works.
 *   - Goes NO_SIGNAL after a 60 s sticky window.
 *
 * Replace parse_frame() with the real parser (FSD §11.6 Phase B).
 */

#include "decoder_dfm.h"
#include "rf_sx1276.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sonde_types.h"

static const char *TAG = "dfm";

/* DFM frame: 280 raw bits (35 bytes) data, Manchester chip-rate at
   4800 chips/s actual bitrate (2500 bps data ⇒ 5000 cps). With the
   profile payload_len=256 chip bytes we capture ample data for the
   skeleton — the real parser will resync via the 24-bit sync vector. */
#define CHIP_BYTES           256
#define DATA_BYTES           128
#define IDLE_FRAMES_NO_BYTES 4   /* DFM frames ~~half a second apart */

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

/* Skeleton parse: demonstrate the chip stream is alive, publish a
   placeholder name. Real parser will validate the 24-bit sync, run
   the de-interleaver, apply Hamming FEC, and fill lat/lon/alt. */
static bool parse_frame(const uint8_t *chips, sonde_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type         = SONDE_TYPE_DFM;
    out->rssi_dbm     = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();

    uint8_t frame[DATA_BYTES] = {0};
    manchester_decode(chips, frame);

    /* No CRC available without the real parser — accept any chip
       buffer so the user sees the stream is being captured. */
    snprintf(out->name, sizeof(out->name), "DFM-%02X%02X",
             frame[0], frame[1]);
    out->state = SONDE_STATE_NAME_ONLY;
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
                    ESP_LOGI(TAG, "skeleton frame: %s", f.name);
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
                f.type         = SONDE_TYPE_DFM;
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

static esp_err_t dfm_start(QueueHandle_t byte_q)
{
    if (s_running) return ESP_OK;
    s_q       = byte_q;
    s_running = true;

    const rf_profile_t *p = st_rf_profile_for(SONDE_TYPE_DFM);
    if (p) st_rf_apply_profile(p);

    BaseType_t r = xTaskCreate(task, "dfm", 8192, NULL, 10, &s_task);
    if (r != pdPASS) { s_running = false; return ESP_FAIL; }
    ESP_LOGI(TAG, "decoder started (skeleton)");
    return ESP_OK;
}

static esp_err_t dfm_stop(void)
{
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

static const decoder_vtable_t s_vtable = {
    .type  = SONDE_TYPE_DFM,
    .name  = "dfm",
    .start = dfm_start,
    .stop  = dfm_stop,
};

const decoder_vtable_t *decoder_dfm_vtable(void) { return &s_vtable; }
