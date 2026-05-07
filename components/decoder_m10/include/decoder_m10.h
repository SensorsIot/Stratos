#pragma once

#include "decoder_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Meteomodem M10 frame decoder. Returns the decoder vtable for
   registration with decoder_core. */
const decoder_vtable_t *decoder_m10_vtable(void);

#ifdef __cplusplus
}
#endif
