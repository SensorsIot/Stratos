#include "platform_common.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "log";

static int               s_udp_sock = -1;
static struct sockaddr_in s_udp_addr;
static SemaphoreHandle_t s_udp_lock;
static vprintf_like_t    s_chained_vprintf;

static int udp_vprintf(const char *fmt, va_list ap)
{
    int n = vprintf(fmt, ap);
    if (s_udp_sock >= 0) {
        char line[256];
        va_list ap2;
        va_copy(ap2, ap);
        int len = vsnprintf(line, sizeof(line), fmt, ap2);
        va_end(ap2);
        if (len > 0) {
            if (len > (int)sizeof(line)) len = sizeof(line);
            if (xSemaphoreTake(s_udp_lock, 0) == pdTRUE) {
                sendto(s_udp_sock, line, len, 0,
                       (struct sockaddr *)&s_udp_addr, sizeof(s_udp_addr));
                xSemaphoreGive(s_udp_lock);
            }
        }
    }
    return n;
}

esp_err_t st_logging_init(void)
{
    if (!s_udp_lock) {
        s_udp_lock = xSemaphoreCreateMutex();
    }
    s_chained_vprintf = esp_log_set_vprintf(udp_vprintf);
    ESP_LOGI(TAG, "logging initialised");
    return ESP_OK;
}

esp_err_t st_logging_set_udp_target(const char *host, uint16_t port)
{
    if (!s_udp_lock) st_logging_init();
    xSemaphoreTake(s_udp_lock, portMAX_DELAY);
    if (s_udp_sock >= 0) {
        close(s_udp_sock);
        s_udp_sock = -1;
    }
    if (host && host[0] && port != 0) {
        s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp_sock < 0) {
            xSemaphoreGive(s_udp_lock);
            return ESP_FAIL;
        }
        memset(&s_udp_addr, 0, sizeof(s_udp_addr));
        s_udp_addr.sin_family = AF_INET;
        s_udp_addr.sin_port = htons(port);
        if (inet_aton(host, &s_udp_addr.sin_addr) == 0) {
            close(s_udp_sock);
            s_udp_sock = -1;
            xSemaphoreGive(s_udp_lock);
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGI(TAG, "UDP log → %s:%u", host, port);
    }
    xSemaphoreGive(s_udp_lock);
    return ESP_OK;
}
