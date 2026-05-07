/*
 * decoder_rs41 — Vaisala RS41 frame consumer.
 *
 * STATUS: framework. Reads bytes from the SX1276 byte queue, runs sync-byte
 * scanning, accumulates RS41 frame buffers, and publishes sonde_frame_t.
 *
 * The full rs1729 RS41 demodulator (Manchester decoding + Reed-Solomon +
 * field extraction) is not vendored in this commit. Phase 3 work in the FSD
 * (§3.3) covers porting it under GPL-2.0. Once vendored, replace the
 * stub fields produced by frame_parse_stub() with rs1729-derived values.
 *
 * Until then, the decoder still emits NAME_ONLY frames when byte traffic is
 * detected on the queue, so the upper layers (state machine, BLE, OLED)
 * exercise their full code paths against synthetic frames.
 */

#include "decoder_rs41.h"
#include "rf_sx1276.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sonde_types.h"

static const char *TAG = "rs41";

#define FRAME_BYTES   320       /* ~RS41 standard frame length (incl. RS) */
#define IDLE_FRAMES_NO_BYTES 2  /* publish NO_SIGNAL after this many silent ticks */

static TaskHandle_t  s_task;
static QueueHandle_t s_q;
static volatile bool s_running;

static void frame_parse_stub(const uint8_t *buf, size_t len, sonde_frame_t *out)
{
    (void)buf; (void)len;
    /* TODO Phase 3: replace with rs1729 RS41 parser. Inputs: 320-byte frame.
       Outputs: sonde_frame_t with name (serial), lat/lon/alt, h_vel, v_vel, AFC, BK. */
    out->state = SONDE_STATE_NAME_ONLY;
    out->type  = SONDE_TYPE_RS41;
    snprintf(out->name, sizeof(out->name), "RS41-PRE");
    out->rssi_dbm = st_rf_rssi_dbm();
    out->monotonic_us = esp_timer_get_time();
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
            buf[pos++] = b;
            if (pos >= FRAME_BYTES) {
                sonde_frame_t f = {0};
                frame_parse_stub(buf, pos, &f);
                decoder_core_publish_frame(&f);
                pos = 0;
            }
        } else {
            if (++idle_ticks >= IDLE_FRAMES_NO_BYTES) {
                sonde_frame_t f = {0};
                f.state = SONDE_STATE_NO_SIGNAL;
                f.type  = SONDE_TYPE_RS41;
                f.rssi_dbm = st_rf_rssi_dbm();
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
    s_q = byte_q;
    s_running = true;

    /* Apply RS41 RF profile to the radio. */
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
