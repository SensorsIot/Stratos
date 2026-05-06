/*
 * Balloon-Receiver — RS41 radiosonde receiver firmware (skeleton)
 *
 * See docs/fsd/MySondyGo-IDF FSD.md for the full specification.
 *
 * Copyright (C) 2026 SensorsIot.
 * Licensed under the GNU GPL v2.0. Includes derivative work of rs1729/RS (GPL-2.0).
 */

#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "balloon-receiver";

void app_main(void)
{
    ESP_LOGI(TAG, "Balloon-Receiver booting (skeleton — see docs/fsd)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS ready. Components TBD per FSD Phase 1.");
}
