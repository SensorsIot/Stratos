#include "decoder_core.h"

#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"

static const char *TAG = "dec_core";

#define MAX_DECODERS 8
static const decoder_vtable_t *s_table[MAX_DECODERS];
static int                    s_count;
static const decoder_vtable_t *s_active;

esp_err_t decoder_core_register(const decoder_vtable_t *v)
{
    if (!v) return ESP_ERR_INVALID_ARG;
    if (s_count >= MAX_DECODERS) return ESP_ERR_NO_MEM;
    s_table[s_count++] = v;
    ESP_LOGI(TAG, "registered: %s", v->name);
    return ESP_OK;
}

esp_err_t decoder_core_set_active(sonde_type_t t, QueueHandle_t byte_q)
{
    if (s_active && s_active->stop) s_active->stop();
    s_active = NULL;
    for (int i = 0; i < s_count; i++) {
        if (s_table[i]->type == t) {
            s_active = s_table[i];
            if (s_active->start) {
                esp_err_t e = s_active->start(byte_q);
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "%s start: %s", s_active->name, esp_err_to_name(e));
                    s_active = NULL;
                    return e;
                }
            }
            ESP_LOGI(TAG, "active: %s", s_active->name);
            return ESP_OK;
        }
    }
    ESP_LOGI(TAG, "no decoder for type=%d (NO_SIGNAL only)", t);
    return ESP_OK;
}

sonde_type_t decoder_core_active(void)
{
    return s_active ? s_active->type : SONDE_TYPE_UNKNOWN;
}

void decoder_core_publish_frame(const sonde_frame_t *f)
{
    if (!f) return;
    sonde_frame_t copy = *f;
    if (copy.monotonic_us == 0) copy.monotonic_us = esp_timer_get_time();
    esp_event_post(SONDE_EVENT, SONDE_EVT_FRAME, &copy, sizeof(copy), 0);
}
