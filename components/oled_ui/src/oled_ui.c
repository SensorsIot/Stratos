#include "oled_ui.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "esp_event.h"

#include "board_profile.h"
#include "config_store.h"
#include "platform_common.h"
#include "sonde_types.h"
#include "rf_sx1276.h"

#include "font5x7.h"

/*
 * SSD1306 driver using the IDF v5.5 i2c_master API.
 *
 * Validated standalone on LilyGO T3 V1.6 — see /home/dev/blink_test/.
 * The earlier "i2c_master fires INT WDT under WiFi+BT" diagnosis was wrong;
 * the real culprit was driving GPIO 16 as a non-existent OLED reset, which
 * glitched the rail (rst:0x1 POWERON_RESET). On this board oled_rst is
 * GPIO_NUM_NC and we never touch a reset line.
 */

#define SSD1306_ADDR  0x3C
#define WIDTH  128
#define HEIGHT 64
#define PAGES  (HEIGHT / 8)
#define I2C_HZ 400000

static const char *TAG = "oled";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t    s_fb[WIDTH * PAGES];
static volatile bool s_on = true;
static volatile bool s_ready;
static sonde_frame_t s_last_frame;
/* Stickiness: keep the last NAME_ONLY / TRACKING frame visible for a few
   seconds after signal drops, so the OLED doesn't flicker between the
   real sonde ID and "-- searching --" each second. */
static sonde_frame_t s_last_locked_frame;
static int64_t       s_last_locked_us;
#define STICKY_LOCK_US (10LL * 1000000LL)   /* 10 s */

static esp_err_t send_cmd(uint8_t c)
{
    uint8_t buf[2] = { 0x00, c };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t send_data(const uint8_t *data, size_t n)
{
    uint8_t hdr = 0x40;
    i2c_master_transmit_multi_buffer_info_t bufs[2] = {
        { .write_buffer = &hdr, .buffer_size = 1 },
        { .write_buffer = (uint8_t *)data, .buffer_size = n },
    };
    return i2c_master_multi_buffer_transmit(s_dev, bufs, 2, 100);
}

static void flush(void)
{
    if (!s_ready) return;
    for (int page = 0; page < PAGES; page++) {
        if (send_cmd(0xB0 | page) != ESP_OK) return;
        send_cmd(0x00);
        send_cmd(0x10);
        if (send_data(&s_fb[page * WIDTH], WIDTH) != ESP_OK) return;
    }
}

void st_oled_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

static void put_char(int x, int y_page, char c)
{
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = font5x7[c - 0x20];
    for (int i = 0; i < 5 && x + i < WIDTH; i++) {
        s_fb[y_page * WIDTH + x + i] = g[i];
    }
    if (x + 5 < WIDTH) s_fb[y_page * WIDTH + x + 5] = 0;
}

static void put_str(int x, int y_page, const char *s)
{
    while (*s && x < WIDTH) {
        put_char(x, y_page, *s++);
        x += 6;
    }
}

static esp_err_t oled_bring_up(void)
{
    const board_profile_t *bp = board_profile_active();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = bp->oled_sda,
        .scl_io_num = bp->oled_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(s_bus, SSD1306_ADDR, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SSD1306 ACK at 0x%02X (%s)", SSD1306_ADDR, esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = SSD1306_ADDR,
        .scl_speed_hz    = I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    /* Page addressing mode (0x20, 0x02) matches our per-page flush below. */
    static const uint8_t init_seq[] = {
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x02,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF,
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        esp_err_t e = send_cmd(init_seq[i]);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "init cmd %u (byte 0x%02X) NACK: %s",
                     (unsigned)i, init_seq[i], esp_err_to_name(e));
            return e;
        }
    }
    s_ready = true;
    st_oled_clear();
    flush();
    ESP_LOGI(TAG, "SSD1306 initialised (i2c_master, %d Hz)", I2C_HZ);
    return ESP_OK;
}

void st_oled_set_power(bool on)
{
    s_on = on;
    if (s_ready) send_cmd(on ? 0xAF : 0xAE);
}

bool st_oled_is_on(void) { return s_on; }

void st_oled_render_boot(const char *ssid, const char *version)
{
    if (!s_ready) return;
    st_oled_clear();
    put_str(0,  0, "Stratos Receiver");
    put_str(0,  2, "AP:");
    put_str(24, 2, ssid ? ssid : "(starting)");
    put_str(0,  4, "URL: 192.168.4.1");
    put_str(0,  6, "v");
    put_str(8,  6, version ? version : "?");
    flush();
}

static void on_sonde_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == SONDE_EVT_FRAME && data) {
        const sonde_frame_t *f = (const sonde_frame_t *)data;
        s_last_frame = *f;
        if (f->state == SONDE_STATE_TRACKING || f->state == SONDE_STATE_NAME_ONLY) {
            s_last_locked_frame = *f;
            s_last_locked_us    = f->monotonic_us;
        }
    }
}

static const char *type_name(sonde_type_t t)
{
    switch (t) {
        case SONDE_TYPE_RS41:  return "RS41";
        case SONDE_TYPE_M20:   return "M20";
        case SONDE_TYPE_M10:   return "M10";
        case SONDE_TYPE_PILOT: return "PILOT";
        case SONDE_TYPE_DFM:   return "DFM";
        default:               return "?";
    }
}

static void render_status(void)
{
    char line[32];
    st_config_t c = st_config_get();
    sonde_frame_t cur = s_last_frame;

    /* Pick the frame to display: prefer current if it has a lock, otherwise
       fall back to the last known-good frame for STICKY_LOCK_US so the
       display doesn't flicker between the real ID and "-- searching --". */
    int64_t now_us = esp_timer_get_time();
    sonde_frame_t f = cur;
    if (cur.state != SONDE_STATE_TRACKING && cur.state != SONDE_STATE_NAME_ONLY &&
        s_last_locked_us != 0 &&
        (now_us - s_last_locked_us) < STICKY_LOCK_US) {
        f = s_last_locked_frame;
        f.rssi_dbm = cur.rssi_dbm;   /* keep RSSI live */
    }

    st_oled_clear();
    snprintf(line, sizeof(line), "%-5s %3lu.%03lu MHz",
             type_name(c.sonde_type),
             (unsigned long)(c.freq_khz / 1000),
             (unsigned long)(c.freq_khz % 1000));
    put_str(0, 0, line);

    const char *st = "NO SIGNAL";
    if (f.state == SONDE_STATE_TRACKING)  st = "TRACKING";
    else if (f.state == SONDE_STATE_NAME_ONLY) st = "NAME";
    put_str(0, 2, st);

    if (f.state == SONDE_STATE_TRACKING || f.state == SONDE_STATE_NAME_ONLY) {
        snprintf(line, sizeof(line), "ID %.10s", f.name);
        put_str(0, 3, line);
        snprintf(line, sizeof(line), "%+8.4f %+8.4f", f.lat, f.lon);
        put_str(0, 4, line);
        snprintf(line, sizeof(line), "ALT %ld m  V %+.1f", (long)f.alt_m, f.v_vel_ms);
        put_str(0, 5, line);
    } else {
        put_str(0, 3, "-- searching --");
    }
    snprintf(line, sizeof(line), "RSSI %d dBm", f.rssi_dbm);
    put_str(0, 6, line);
    /* Diagnostic line (line 7): byte count and sync match count from the
       SX1276 driver. Lets us see at a glance whether the front-end is
       producing data even when state stays NO_SIGNAL. */
    snprintf(line, sizeof(line), "B%lu S%lu",
             (unsigned long)st_rf_byte_count(),
             (unsigned long)st_rf_sync_count());
    put_str(0, 7, line);

    flush();
}

static void oled_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_err_t err = oled_bring_up();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed (%s) — running headless", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    st_config_t c = st_config_get();
    st_oled_set_power(c.oled_persist);
    esp_event_handler_register(SONDE_EVENT, SONDE_EVT_FRAME, on_sonde_evt, NULL);
    for (;;) {
        if (s_on && s_ready) render_status();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t st_oled_init(void)
{
    BaseType_t r = xTaskCreate(oled_task, "oled", 4096, NULL, 1, NULL);
    return r == pdPASS ? ESP_OK : ESP_FAIL;
}

void st_oled_start_renderer(void) { /* render loop is in oled_task */ }
