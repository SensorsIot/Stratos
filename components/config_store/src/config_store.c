#include "config_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cfg";
static const char *NS  = "mysongo";

static st_config_t s_cfg;
static SemaphoreHandle_t s_lock;

static const st_config_t s_defaults = {
    .board_id        = 0,
    .freq_khz        = 404000,
    .sonde_type      = SONDE_TYPE_RS41,
    .mycall          = "MYCALL",
    .ble_on          = true,
    .oled_persist    = true,
    .freqofs_hz      = 0,
    .rxbw_idx_rs41   = 4,
    .rxbw_idx_m20    = 7,
    .rxbw_idx_m10    = 7,
    .rxbw_idx_pilot  = 7,
    .rxbw_idx_dfm    = 6,
    .vbat_min_mv     = 2950,
    .vbat_max_mv     = 4200,
    .vbat_type       = 1,
    .aprs_name       = false,
    .mute            = false,
    .udp_log_host    = "",
    .udp_log_port    = 0,
};

static esp_err_t load_field_u32(nvs_handle_t h, const char *k, uint32_t *out)
{
    return nvs_get_u32(h, k, out);
}

static void load_str(nvs_handle_t h, const char *k, char *dst, size_t maxlen)
{
    size_t sz = maxlen;
    if (nvs_get_str(h, k, dst, &sz) != ESP_OK) {
        dst[0] = '\0';
    }
}

static esp_err_t st_config_load_locked(void)
{
    s_cfg = s_defaults;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no NVS namespace yet, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open: %s — defaults", esp_err_to_name(err));
        return ESP_OK;
    }
    uint32_t u32;
    uint16_t u16;
    uint8_t  u8;
    int32_t  i32;
    if (nvs_get_u8(h,  "board_id",     &u8) == ESP_OK) s_cfg.board_id = u8;
    if (load_field_u32(h, "freq_khz",  &u32) == ESP_OK) s_cfg.freq_khz = u32;
    if (nvs_get_u8(h,  "sonde_type",   &u8) == ESP_OK) s_cfg.sonde_type = (sonde_type_t)u8;
    if (nvs_get_u8(h,  "ble_on",       &u8) == ESP_OK) s_cfg.ble_on = u8 != 0;
    if (nvs_get_u8(h,  "oled_persist", &u8) == ESP_OK) s_cfg.oled_persist = u8 != 0;
    if (nvs_get_i32(h, "freqofs_hz",   &i32) == ESP_OK) s_cfg.freqofs_hz = i32;
    if (nvs_get_u8(h,  "rxbw_rs41",    &u8) == ESP_OK) s_cfg.rxbw_idx_rs41 = u8;
    if (nvs_get_u8(h,  "rxbw_m20",     &u8) == ESP_OK) s_cfg.rxbw_idx_m20 = u8;
    if (nvs_get_u8(h,  "rxbw_m10",     &u8) == ESP_OK) s_cfg.rxbw_idx_m10 = u8;
    if (nvs_get_u8(h,  "rxbw_pilot",   &u8) == ESP_OK) s_cfg.rxbw_idx_pilot = u8;
    if (nvs_get_u8(h,  "rxbw_dfm",     &u8) == ESP_OK) s_cfg.rxbw_idx_dfm = u8;
    if (nvs_get_u16(h, "vbat_min",     &u16) == ESP_OK) s_cfg.vbat_min_mv = u16;
    if (nvs_get_u16(h, "vbat_max",     &u16) == ESP_OK) s_cfg.vbat_max_mv = u16;
    if (nvs_get_u8(h,  "vbat_type",    &u8) == ESP_OK) s_cfg.vbat_type = u8;
    if (nvs_get_u8(h,  "aprs_name",    &u8) == ESP_OK) s_cfg.aprs_name = u8 != 0;
    if (nvs_get_u8(h,  "mute",         &u8) == ESP_OK) s_cfg.mute = u8 != 0;
    if (nvs_get_u16(h, "udp_log_port", &u16) == ESP_OK) s_cfg.udp_log_port = u16;
    load_str(h, "mycall",        s_cfg.mycall,       sizeof(s_cfg.mycall));
    load_str(h, "udp_log_host",  s_cfg.udp_log_host, sizeof(s_cfg.udp_log_host));
    nvs_close(h);
    if (s_cfg.mycall[0] == '\0') strcpy(s_cfg.mycall, "MYCALL");
    return ESP_OK;
}

static esp_err_t st_config_persist_locked(const st_config_t *c)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8 (h, "board_id",     c->board_id);
    nvs_set_u32(h, "freq_khz",     c->freq_khz);
    nvs_set_u8 (h, "sonde_type",   (uint8_t)c->sonde_type);
    nvs_set_u8 (h, "ble_on",       c->ble_on ? 1 : 0);
    nvs_set_u8 (h, "oled_persist", c->oled_persist ? 1 : 0);
    nvs_set_i32(h, "freqofs_hz",   c->freqofs_hz);
    nvs_set_u8 (h, "rxbw_rs41",    c->rxbw_idx_rs41);
    nvs_set_u8 (h, "rxbw_m20",     c->rxbw_idx_m20);
    nvs_set_u8 (h, "rxbw_m10",     c->rxbw_idx_m10);
    nvs_set_u8 (h, "rxbw_pilot",   c->rxbw_idx_pilot);
    nvs_set_u8 (h, "rxbw_dfm",     c->rxbw_idx_dfm);
    nvs_set_u16(h, "vbat_min",     c->vbat_min_mv);
    nvs_set_u16(h, "vbat_max",     c->vbat_max_mv);
    nvs_set_u8 (h, "vbat_type",    c->vbat_type);
    nvs_set_u8 (h, "aprs_name",    c->aprs_name ? 1 : 0);
    nvs_set_u8 (h, "mute",         c->mute ? 1 : 0);
    nvs_set_u16(h, "udp_log_port", c->udp_log_port);
    nvs_set_str(h, "mycall",       c->mycall);
    nvs_set_str(h, "udp_log_host", c->udp_log_host);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t st_config_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = st_config_load_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "loaded: freq=%lu kHz tipo=%d mycall=%s ble=%d",
             (unsigned long)s_cfg.freq_khz, s_cfg.sonde_type, s_cfg.mycall, s_cfg.ble_on);
    return err;
}

st_config_t st_config_get(void)
{
    st_config_t copy;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy = s_cfg;
    xSemaphoreGive(s_lock);
    return copy;
}

esp_err_t st_config_save(const st_config_t *cfg, uint32_t changed_mask)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    esp_err_t err = st_config_persist_locked(&s_cfg);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        esp_event_post(CFG_EVENT, CFG_EVT_CHANGED, &changed_mask, sizeof(changed_mask),
                       portMAX_DELAY);
    }
    return err;
}

esp_err_t st_config_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = s_defaults;
    xSemaphoreGive(s_lock);
    uint32_t mask = 0xFFFFFFFFu;
    esp_event_post(CFG_EVENT, CFG_EVT_FACTORY_RESET, &mask, sizeof(mask), portMAX_DELAY);
    return ESP_OK;
}

esp_err_t st_config_set_freq_khz(uint32_t khz)
{
    if (khz < 137200 || khz > 524800) return ESP_ERR_INVALID_ARG;
    st_config_t c = st_config_get();
    if (c.freq_khz == khz) return ESP_OK;
    c.freq_khz = khz;
    return st_config_save(&c, CFG_FIELD_FREQ);
}

esp_err_t st_config_set_sonde_type(sonde_type_t t)
{
    if (t < SONDE_TYPE_RS41 || t > SONDE_TYPE_DFM) return ESP_ERR_INVALID_ARG;
    st_config_t c = st_config_get();
    if (c.sonde_type == t) return ESP_OK;
    c.sonde_type = t;
    return st_config_save(&c, CFG_FIELD_SONDE_TYPE);
}
