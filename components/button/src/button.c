#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "board_profile.h"

static const char *TAG = "btn";

#define POLL_MS       20
#define DEBOUNCE_MS   50
#define LONG_MS     5000

static st_btn_handler_t s_cb;
static void            *s_ctx;

static void task(void *arg)
{
    (void)arg;
    const board_profile_t *bp = board_profile_active();
    gpio_config_t g = {
        .pin_bit_mask = 1ULL << bp->button,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&g);

    int hold_ms = 0;
    bool prev = true;          /* button is pulled up; pressed = 0 */
    bool stable = true;
    int  same_ms = 0;
    bool long_fired = false;

    for (;;) {
        bool raw = gpio_get_level(bp->button) != 0;
        if (raw == prev) same_ms += POLL_MS;
        else { prev = raw; same_ms = 0; }

        if (same_ms >= DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (!stable) {
                hold_ms = 0;
                long_fired = false;
            } else {
                if (!long_fired && hold_ms < LONG_MS) {
                    if (s_cb) s_cb(ST_BTN_SHORT, s_ctx);
                }
            }
        }
        if (!stable) {
            hold_ms += POLL_MS;
            if (!long_fired && hold_ms >= LONG_MS) {
                long_fired = true;
                ESP_LOGI(TAG, "long press");
                if (s_cb) s_cb(ST_BTN_LONG, s_ctx);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t st_button_init(st_btn_handler_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    BaseType_t r = xTaskCreate(task, "btn", 2560, NULL, 4, NULL);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}
