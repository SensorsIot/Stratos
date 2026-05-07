#include "platform_common.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

static const char *TAG = "wdt";

#define HEAP_WARN_BYTES 32768
#define HEAP_CRIT_BYTES  8192

esp_err_t st_watchdog_init(void)
{
    esp_task_wdt_config_t cfg = {
        .timeout_ms = 60 * 1000,
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_err_t err = esp_task_wdt_reconfigure(&cfg);
    if (err == ESP_ERR_INVALID_STATE) {
        err = esp_task_wdt_init(&cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wdt init: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t st_watchdog_register_current_task(void)
{
    return esp_task_wdt_add(NULL);
}

void st_watchdog_feed(void)
{
    esp_task_wdt_reset();
}

static void heap_monitor_task(void *arg)
{
    (void)arg;
    bool warned = false;
    for (;;) {
        size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (free_b < HEAP_CRIT_BYTES) {
            ESP_LOGE(TAG, "heap critical (%u B) — rebooting", (unsigned)free_b);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else if (free_b < HEAP_WARN_BYTES && !warned) {
            ESP_LOGW(TAG, "heap low (%u B)", (unsigned)free_b);
            warned = true;
        } else if (free_b >= HEAP_WARN_BYTES + 4096) {
            warned = false;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t st_heap_monitor_start(void)
{
    BaseType_t r = xTaskCreate(heap_monitor_task, "heap_mon", 2048, NULL, 3, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_FAIL;
}
