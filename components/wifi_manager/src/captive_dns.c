/* Tiny captive-portal DNS responder: every A query → 192.168.4.1. */

#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "captdns";
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

static void dns_task(void *arg)
{
    (void)arg;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr   = { .s_addr = htonl(INADDR_ANY) },
    };
    if (bind(s, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(s);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS listening on UDP/53");

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &plen);
        if (n < 12) continue;
        buf[2] = 0x81; buf[3] = 0x80;     /* response, recursion avail */
        buf[6] = 0x00; buf[7] = 0x01;     /* answers = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;

        int qend = 12;
        while (qend < n && buf[qend] != 0) qend += buf[qend] + 1;
        qend += 5;                         /* terminator + qtype + qclass */
        if (qend + 16 > (int)sizeof(buf)) continue;

        int p = qend;
        buf[p++] = 0xC0; buf[p++] = 0x0C;  /* pointer to question name */
        buf[p++] = 0x00; buf[p++] = 0x01;  /* type A */
        buf[p++] = 0x00; buf[p++] = 0x01;  /* class IN */
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;  /* TTL 60 */
        buf[p++] = 0x00; buf[p++] = 0x04;  /* rdlength */
        buf[p++] = AP_IP[0]; buf[p++] = AP_IP[1];
        buf[p++] = AP_IP[2]; buf[p++] = AP_IP[3];

        sendto(s, buf, p, 0, (struct sockaddr *)&peer, plen);
    }
}

void st_captive_dns_start(void)
{
    xTaskCreate(dns_task, "capt_dns", 3072, NULL, 4, NULL);
}
