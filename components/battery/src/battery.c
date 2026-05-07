#include "battery.h"

#include <math.h>

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_profile.h"
#include "config_store.h"

static const char *TAG = "bat";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int                       s_bat_mv = 0;
static int                       s_bat_pct = -1;

#define WINDOW 16

static int curve_pct(int mv, int vmin, int vmax, int type)
{
    if (mv <= 0) return -1;
    if (vmax <= vmin) return 0;
    float x = (float)(mv - vmin) / (float)(vmax - vmin);
    if (x < 0) x = 0;
    if (x > 1) x = 1;
    float y;
    switch (type) {
        case 0:  y = x; break;
        case 2:  /* Anti-sigmoid: steep ends, slow middle */
                 y = (x < 0.5f) ? (0.5f * powf(2.0f * x, 2.5f))
                                : (1.0f - 0.5f * powf(2.0f * (1.0f - x), 2.5f));
                 break;
        default: /* sigmoidal: 1/(1+exp(-k(x-0.5))) */
                 y = 1.0f / (1.0f + expf(-12.0f * (x - 0.5f)));
                 break;
    }
    int p = (int)(y * 100.0f + 0.5f);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

static void task(void *arg)
{
    (void)arg;
    int hist[WINDOW] = {0};
    int n = 0, i = 0;
    for (;;) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_7, &raw) == ESP_OK) {
            if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
                mv *= 2;            /* on-board 2:1 divider */
            } else {
                mv = raw * 2 * 3300 / 4095;
            }
        }
        hist[i] = mv;
        i = (i + 1) % WINDOW;
        if (n < WINDOW) n++;
        long sum = 0;
        for (int k = 0; k < n; k++) sum += hist[k];
        int avg = (int)(sum / n);

        if (avg < 2000) {
            s_bat_mv = 0;
            s_bat_pct = -1;
        } else {
            s_bat_mv = avg;
            st_config_t c = st_config_get();
            s_bat_pct = curve_pct(avg, c.vbat_min_mv, c.vbat_max_mv, c.vbat_type);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t st_battery_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) return err;

    adc_oneshot_chan_cfg_t ccfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_adc, ADC_CHANNEL_7, &ccfg);

#if CONFIG_IDF_TARGET_ESP32
    adc_cali_line_fitting_config_t lf = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&lf, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no eFuse calibration; using nominal scale");
        s_cali = NULL;
    }
#endif
    xTaskCreate(task, "bat", 2560, NULL, 3, NULL);
    return ESP_OK;
}

int st_battery_mv(void)  { return s_bat_mv; }
int st_battery_pct(void) { return s_bat_pct; }
