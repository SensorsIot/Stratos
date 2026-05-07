#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_VERSION_MAJOR 1
#define ST_VERSION_MINOR 0
#define ST_VERSION_PATCH 0

const char *st_version_string(void);
const char *st_git_sha_short(void);

esp_err_t st_logging_init(void);
esp_err_t st_logging_set_udp_target(const char *host, uint16_t port);

esp_err_t st_watchdog_init(void);
esp_err_t st_watchdog_register_current_task(void);
void      st_watchdog_feed(void);

esp_err_t st_heap_monitor_start(void);

#ifdef __cplusplus
}
#endif
