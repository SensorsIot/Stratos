#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"

static const char *TAG = "wifi";
static char         s_ssid[32];
static esp_netif_t *s_ap_netif;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "client connected");
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "client disconnected");
        break;
    default:
        break;
    }
}

esp_err_t st_wifi_start_ap(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ssid, sizeof(s_ssid), "Stratos-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len       = strlen(s_ssid);
    ap.ap.channel        = 6;
    ap.ap.authmode       = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ap.ap.beacon_interval = 100;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AP up: SSID=%s, IP=192.168.4.1", s_ssid);
    return ESP_OK;
}

const char *st_wifi_ssid(void) { return s_ssid; }
esp_netif_t *st_wifi_ap_netif(void) { return s_ap_netif; }
