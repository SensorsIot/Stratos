#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "sonde_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*msg_emit_fn)(const char *bytes, size_t len, void *ctx);

void msg_codec_init(msg_emit_fn emit, void *ctx);

void msg_codec_emit_state(const sonde_frame_t *f);
void msg_codec_emit_settings(void);

esp_err_t msg_codec_handle_input(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
