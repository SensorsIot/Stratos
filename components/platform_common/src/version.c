#include "platform_common.h"
#include "esp_app_desc.h"
#include <stdio.h>
#include <string.h>

static char s_version_buf[40];
static char s_sha_buf[16];

const char *st_version_string(void)
{
    if (s_version_buf[0]) return s_version_buf;
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_version_buf, sizeof(s_version_buf), "%d.%d.%d+%.7s",
             ST_VERSION_MAJOR, ST_VERSION_MINOR, ST_VERSION_PATCH,
             d->version[0] ? d->version : "dev");
    return s_version_buf;
}

const char *st_git_sha_short(void)
{
    if (s_sha_buf[0]) return s_sha_buf;
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_sha_buf, sizeof(s_sha_buf), "%.7s", d->version[0] ? d->version : "dev");
    return s_sha_buf;
}
