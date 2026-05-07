/*
 * Stratos — RS41 weather-balloon receiver firmware (BLE companion to BalloonHunter).
 * See docs/fsd/Stratos FSD.md for the full specification.
 *
 * Copyright (C) 2026 SensorsIot. Licensed under GPL-2.0.
 * Includes derivative work of rs1729/RS (GPL-2.0).
 */

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "platform_common.h"
#include "config_store.h"
#include "board_profile.h"
#include "wifi_manager.h"
#include "http_ui.h"
#include "st_ota.h"
#include "oled_ui.h"
#include "button.h"
#include "battery.h"
#include "ble_nus.h"
#include "rf_sx1276.h"
#include "decoder_core.h"
#include "decoder_rs41.h"
#include "sonde_state.h"

static const char *TAG = "stratos";

static void on_button(st_btn_event_t evt, void *ctx)
{
    (void)ctx;
    if (evt == ST_BTN_SHORT) {
        bool now = !st_oled_is_on();
        st_oled_set_power(now);
        st_config_t c = st_config_get();
        if (c.oled_persist != now) {
            c.oled_persist = now;
            st_config_save(&c, CFG_FIELD_OLED_PERSIST);
        }
    } else if (evt == ST_BTN_LONG) {
        ESP_LOGW(TAG, "long press → factory reset");
        extern void st_factory_reset_and_reboot(int ms);
        st_factory_reset_and_reboot(500);
    }
}

static void on_cfg_change(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != CFG_EVT_CHANGED || !data) return;
    uint32_t mask = *(uint32_t *)data;
    st_config_t c = st_config_get();

    if (mask & CFG_FIELD_FREQ) {
        st_rf_set_freq_hz(c.freq_khz * 1000U);
    }
    if (mask & CFG_FIELD_SONDE_TYPE) {
        decoder_core_set_active(c.sonde_type, st_rf_byte_queue());
        st_rf_set_freq_hz(c.freq_khz * 1000U);
    }
}

static char *make_dev_name(void)
{
    static char name[32];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name, sizeof(name), "Stratos-%02X%02X", mac[4], mac[5]);
    return name;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Stratos %s booting", st_version_string());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    st_logging_init();
    st_config_init();
    st_config_t cfg = st_config_get();
    board_profile_set_active(cfg.board_id);

    if (cfg.udp_log_host[0] && cfg.udp_log_port) {
        st_logging_set_udp_target(cfg.udp_log_host, cfg.udp_log_port);
    }

    /* WiFi AP first so the user always has a way in even if I/O peripherals
       fail later. */
    ESP_LOGI(TAG, "[boot] starting WiFi AP");
    ESP_ERROR_CHECK(st_wifi_start_ap());
    ESP_LOGI(TAG, "[boot] starting HTTP server");
    ESP_ERROR_CHECK(st_http_start());
    st_ota_register(st_http_server());
    ESP_LOGI(TAG, "[boot] starting captive DNS");
    st_captive_dns_start();

    ESP_LOGI(TAG, "[boot] OLED init (deferred task)");
    st_oled_init();

    ESP_LOGI(TAG, "[boot] battery + button");
    st_battery_init();
    st_button_init(on_button, NULL);

    ESP_LOGI(TAG, "[boot] RF init");
    if (st_rf_init() == ESP_OK) {
        st_rf_set_freq_hz(cfg.freq_khz * 1000U);
        decoder_core_register(decoder_rs41_vtable());
        decoder_core_set_active(cfg.sonde_type, st_rf_byte_queue());
        st_rf_start_rx();
    } else {
        ESP_LOGW(TAG, "RF init failed — running without radio");
    }

    ESP_LOGI(TAG, "[boot] sonde state");
    st_state_init();

    ESP_LOGI(TAG, "[boot] BLE init (ble_on=%d)", cfg.ble_on);
    if (cfg.ble_on) {
        st_ble_init(make_dev_name());
    }

    esp_event_handler_register(CFG_EVENT, ESP_EVENT_ANY_ID, on_cfg_change, NULL);

    st_watchdog_init();
    st_heap_monitor_start();

    ESP_LOGI(TAG, "boot complete (free heap %d B)", (int)esp_get_free_heap_size());
}
