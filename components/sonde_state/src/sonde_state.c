#include "sonde_state.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "state";

#define LOCK_LOSS_US (30LL * 1000 * 1000)  /* 30 s per FR-3.2 */

static sonde_frame_t s_last;
static int64_t       s_last_valid_us;

static void on_frame(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != SONDE_EVT_FRAME || !data) return;
    const sonde_frame_t *f = data;

    sonde_state_t prev = s_last.state;
    s_last = *f;

    if (f->state == SONDE_STATE_TRACKING || f->state == SONDE_STATE_NAME_ONLY) {
        s_last_valid_us = esp_timer_get_time();
    }
    if (prev != s_last.state) {
        ESP_LOGI(TAG, "state %d → %d", prev, s_last.state);
        esp_event_post(SONDE_EVENT, SONDE_EVT_STATE_CHANGED,
                       &s_last.state, sizeof(s_last.state), 0);
    }
}

static void watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_last.state != SONDE_STATE_NO_SIGNAL && s_last_valid_us != 0) {
            int64_t age = esp_timer_get_time() - s_last_valid_us;
            if (age > LOCK_LOSS_US) {
                ESP_LOGI(TAG, "lock lost (>%lld ms idle)", age / 1000);
                s_last.state = SONDE_STATE_NO_SIGNAL;
                esp_event_post(SONDE_EVENT, SONDE_EVT_LOCK_LOST,
                               NULL, 0, 0);
                esp_event_post(SONDE_EVENT, SONDE_EVT_STATE_CHANGED,
                               &s_last.state, sizeof(s_last.state), 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t st_state_init(void)
{
    esp_event_handler_register(SONDE_EVENT, SONDE_EVT_FRAME, on_frame, NULL);
    BaseType_t r = xTaskCreate(watchdog_task, "state_wdg", 3072, NULL, 8, NULL);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}

const sonde_frame_t *st_state_last(void) { return &s_last; }
sonde_state_t        st_state_current(void) { return s_last.state; }
