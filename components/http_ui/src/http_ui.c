#include "http_ui.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_event.h"

#include "config_store.h"
#include "platform_common.h"
#include "sonde_types.h"
#include "rf_sx1276.h"
#include "decoder_rs41.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static const char *TAG = "http";
static httpd_handle_t s_srv;

/* ---------- Latest sonde frame snapshot for /api/state ---------- */
static sonde_frame_t s_last_frame;

static void on_sonde_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == SONDE_EVT_FRAME && data) {
        s_last_frame = *(sonde_frame_t *)data;
    }
}

/* ---------- Tiny JSON helpers ---------- */
static const char *type_to_str(sonde_type_t t)
{
    switch (t) {
        case SONDE_TYPE_RS41:  return "RS41";
        case SONDE_TYPE_M20:   return "M20";
        case SONDE_TYPE_M10:   return "M10";
        case SONDE_TYPE_PILOT: return "PILOT";
        case SONDE_TYPE_DFM:   return "DFM";
        default:               return "UNKNOWN";
    }
}

static const char *state_to_str(sonde_state_t s)
{
    switch (s) {
        case SONDE_STATE_TRACKING:  return "TRACKING";
        case SONDE_STATE_NAME_ONLY: return "NAME_ONLY";
        default:                    return "NO_SIGNAL";
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_sendstr(req, body);
}

/* ---------- Handlers ---------- */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t h_state(httpd_req_t *req)
{
    char buf[512];
    sonde_frame_t f = s_last_frame;
    st_config_t   c = st_config_get();
    int32_t  ecef_x = 0, ecef_y = 0, ecef_z = 0;
    uint32_t ecef_age_ms = 0xFFFFFFFFu;
    decoder_rs41_last_ecef(&ecef_x, &ecef_y, &ecef_z, &ecef_age_ms);
    int rs_errs = decoder_rs41_last_rs_errs();
    int rs_s1   = decoder_rs41_last_synd1();
    int rs_s2   = decoder_rs41_last_synd2();

    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"state\":\"%s\","
        "\"type\":\"%s\","
        "\"freq_khz\":%lu,"
        "\"name\":\"%s\","
        "\"lat\":%.5f,\"lon\":%.5f,\"alt_m\":%ld,"
        "\"h_vel_kmh\":%.1f,\"v_vel_ms\":%.1f,"
        "\"rssi_dbm\":%d,\"afc_hz\":%ld,"
        "\"bat_pct\":-1,\"bat_mv\":0,"
        "\"rf_bytes\":%lu,\"rf_sync\":%lu,"
        "\"ecef_x\":%ld,\"ecef_y\":%ld,\"ecef_z\":%ld,\"ecef_age_ms\":%lu,"
        "\"rs_errs\":%d,\"rs_s1\":%d,\"rs_s2\":%d,"
        "\"version\":\"%s\","
        "\"uptime_s\":%lld"
        "}",
        state_to_str(f.state),
        type_to_str(c.sonde_type),
        (unsigned long)c.freq_khz,
        f.name,
        f.lat, f.lon, (long)f.alt_m,
        f.h_vel_kmh, f.v_vel_ms,
        f.rssi_dbm, (long)f.afc_hz,
        (unsigned long)st_rf_byte_count(),
        (unsigned long)st_rf_sync_count(),
        (long)ecef_x, (long)ecef_y, (long)ecef_z, (unsigned long)ecef_age_ms,
        rs_errs, rs_s1, rs_s2,
        st_version_string(),
        esp_timer_get_time() / 1000000LL);
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    return send_json(req, buf);
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    char buf[512];
    st_config_t c = st_config_get();
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"board\":\"ttgo_lora32_v21_16\","
        "\"freq_khz\":%lu,"
        "\"sonde_type\":%d,"
        "\"mycall\":\"%s\","
        "\"ble_on\":%s,"
        "\"vbat_min_mv\":%u,"
        "\"vbat_max_mv\":%u,"
        "\"vbat_type\":%u"
        "}",
        (unsigned long)c.freq_khz,
        (int)c.sonde_type,
        c.mycall,
        c.ble_on ? "true" : "false",
        c.vbat_min_mv, c.vbat_max_mv, c.vbat_type);
    if (n < 0 || n >= (int)sizeof(buf)) return ESP_FAIL;
    return send_json(req, buf);
}

/* Naive JSON value extractor — finds "key" and returns ptr to its value start. */
static const char *find_json_field(const char *body, const char *key)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(body, needle);
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    return p;
}

static long json_int(const char *body, const char *key, long fallback)
{
    const char *p = find_json_field(body, key);
    if (!p) return fallback;
    char *end;
    long v = strtol(p, &end, 10);
    return (end == p) ? fallback : v;
}

static bool json_bool(const char *body, const char *key, bool fallback)
{
    const char *p = find_json_field(body, key);
    if (!p) return fallback;
    if (!strncmp(p, "true", 4))  return true;
    if (!strncmp(p, "false", 5)) return false;
    return fallback;
}

static void json_str(const char *body, const char *key, char *dst, size_t maxlen)
{
    const char *p = find_json_field(body, key);
    if (!p || *p != '"') return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < maxlen) {
        dst[i++] = *p++;
    }
    dst[i] = 0;
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    if (req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "config too large");
        return ESP_OK;
    }
    char body[1024];
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        total += n;
    }
    body[total] = 0;

    st_config_t c = st_config_get();
    uint32_t mask = 0;

    long khz = json_int(body, "freq_khz", -1);
    if (khz >= 137200 && khz <= 524800 && khz != (long)c.freq_khz) {
        c.freq_khz = (uint32_t)khz;
        mask |= CFG_FIELD_FREQ;
    }
    long t = json_int(body, "sonde_type", -1);
    if (t >= SONDE_TYPE_RS41 && t <= SONDE_TYPE_DFM && t != c.sonde_type) {
        c.sonde_type = (sonde_type_t)t;
        mask |= CFG_FIELD_SONDE_TYPE;
    }
    char mycall[CFG_MYCALL_MAX] = {0};
    json_str(body, "mycall", mycall, sizeof(mycall));
    if (mycall[0] && strcmp(mycall, c.mycall) != 0) {
        strlcpy(c.mycall, mycall, sizeof(c.mycall));
        mask |= CFG_FIELD_MYCALL;
    }
    bool ble_on = json_bool(body, "ble_on", c.ble_on);
    if (ble_on != c.ble_on) {
        c.ble_on = ble_on;
        mask |= CFG_FIELD_BLE_ON;
    }
    long vmin = json_int(body, "vbat_min_mv", c.vbat_min_mv);
    long vmax = json_int(body, "vbat_max_mv", c.vbat_max_mv);
    long vtype = json_int(body, "vbat_type",  c.vbat_type);
    if (vmin >= 2000 && vmin <= 5000 && (uint16_t)vmin != c.vbat_min_mv) { c.vbat_min_mv = vmin; mask |= CFG_FIELD_BAT; }
    if (vmax >= 2000 && vmax <= 5000 && (uint16_t)vmax != c.vbat_max_mv) { c.vbat_max_mv = vmax; mask |= CFG_FIELD_BAT; }
    if (vtype >= 0 && vtype <= 2 && (uint8_t)vtype != c.vbat_type) { c.vbat_type = vtype; mask |= CFG_FIELD_BAT; }

    if (mask) {
        if (st_config_save(&c, mask) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist failed");
            return ESP_OK;
        }
    }
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_version(httpd_req_t *req)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"%s\",\"github_repo\":\"SensorsIot/Stratos\"}",
             st_version_string());
    return send_json(req, buf);
}

static void reboot_after_ms(int ms)
{
    extern void st_schedule_reboot(int ms);
    st_schedule_reboot(ms);
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    st_config_factory_reset();
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    reboot_after_ms(500);
    return ESP_OK;
}

/* Captive-portal redirect: any 404 → 302 to root. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t st_http_start(void)
{
    if (s_srv) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers   = 16;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.lru_purge_enable   = true;
    cfg.stack_size         = 8192;
    esp_err_t err = httpd_start(&s_srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

#define REG(_uri, _m, _fn) do { \
    httpd_uri_t _u = { .uri = (_uri), .method = (_m), .handler = (_fn), .user_ctx = NULL }; \
    httpd_register_uri_handler(s_srv, &_u); } while (0)

    REG("/",                  HTTP_GET,  h_root);
    REG("/api/state",         HTTP_GET,  h_state);
    REG("/api/config",        HTTP_GET,  h_config_get);
    REG("/api/config",        HTTP_POST, h_config_post);
    REG("/api/version",       HTTP_GET,  h_version);
    REG("/api/factory-reset", HTTP_POST, h_factory_reset);
    /* OTA endpoints are registered separately by main via st_ota_register(). */

    httpd_register_err_handler(s_srv, HTTPD_404_NOT_FOUND, h_404);

    esp_event_handler_register(SONDE_EVENT, SONDE_EVT_FRAME, on_sonde_evt, NULL);
    ESP_LOGI(TAG, "HTTP server up");
    return ESP_OK;
}

httpd_handle_t st_http_server(void) { return s_srv; }
