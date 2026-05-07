#include "st_ota.h"
#include "http_ui.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";

static SemaphoreHandle_t s_progress_lock;
static int               s_progress_pct;
static bool              s_done;
static bool              s_error;
static char              s_error_msg[64];

static void progress_set(int pct, bool done, bool error, const char *msg)
{
    if (!s_progress_lock) return;
    xSemaphoreTake(s_progress_lock, portMAX_DELAY);
    s_progress_pct = pct;
    s_done  = done;
    s_error = error;
    if (msg) strlcpy(s_error_msg, msg, sizeof(s_error_msg));
    xSemaphoreGive(s_progress_lock);
}

static void reboot_task(void *arg)
{
    int ms = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_restart();
}

void st_schedule_reboot(int ms)
{
    xTaskCreate(reboot_task, "reboot", 2048, (void *)(intptr_t)ms, 10, NULL);
}

void st_factory_reset_and_reboot(int ms)
{
    extern esp_err_t st_config_factory_reset(void);
    st_config_factory_reset();
    st_schedule_reboot(ms);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    if (req->content_len < 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image too small");
        return ESP_OK;
    }
    progress_set(0, false, false, NULL);

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (!next) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA slot");
        return ESP_OK;
    }
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    char buf[1024];
    size_t total = 0;
    bool   header_ok = false;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf, sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(h);
            progress_set(0, true, true, "recv failed");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        if (!header_ok && (size_t)n >= sizeof(esp_image_header_t)) {
            const esp_image_header_t *ih = (const esp_image_header_t *)buf;
            if (ih->magic != ESP_IMAGE_HEADER_MAGIC) {
                esp_ota_abort(h);
                progress_set(0, true, true, "bad image header");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image header");
                return ESP_OK;
            }
            header_ok = true;
        }
        err = esp_ota_write(h, buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            progress_set(0, true, true, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_OK;
        }
        total += n;
        int pct = (int)(total * 100 / req->content_len);
        progress_set(pct, false, false, NULL);
    }
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        progress_set(100, true, true, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        progress_set(100, true, true, esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    progress_set(100, true, false, NULL);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "OTA complete (%u bytes) — rebooting", (unsigned)total);
    st_schedule_reboot(500);
    return ESP_OK;
}

static esp_err_t h_progress(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char line[80];
    int last_pct = -1;
    bool last_done = false;
    int  ticks_idle = 0;
    while (ticks_idle < 600) {  /* up to 60 s timeout when nothing in progress */
        int pct; bool done; bool error; char msg[64];
        xSemaphoreTake(s_progress_lock, portMAX_DELAY);
        pct = s_progress_pct; done = s_done; error = s_error;
        strlcpy(msg, s_error_msg, sizeof(msg));
        xSemaphoreGive(s_progress_lock);

        if (pct != last_pct) {
            last_pct = pct;
            int n = snprintf(line, sizeof(line), "event: progress\ndata: {\"pct\":%d}\n\n", pct);
            if (httpd_resp_send_chunk(req, line, n) != ESP_OK) return ESP_OK;
            ticks_idle = 0;
        }
        if (done && !last_done) {
            last_done = true;
            int n = error
                ? snprintf(line, sizeof(line), "event: error\ndata: {\"msg\":\"%s\"}\n\n", msg)
                : snprintf(line, sizeof(line), "event: complete\ndata: {}\n\n");
            httpd_resp_send_chunk(req, line, n);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        ticks_idle++;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t st_ota_register(httpd_handle_t srv)
{
    if (!s_progress_lock) s_progress_lock = xSemaphoreCreateMutex();

    httpd_uri_t up = {
        .uri = "/api/ota/upload", .method = HTTP_POST, .handler = h_upload,
    };
    httpd_register_uri_handler(srv, &up);
    httpd_uri_t pg = {
        .uri = "/api/ota/progress", .method = HTTP_GET, .handler = h_progress,
    };
    httpd_register_uri_handler(srv, &pg);
    return ESP_OK;
}
