#pragma once

#include <stdint.h>
#include "decoder_core.h"

#ifdef __cplusplus
extern "C" {
#endif

const decoder_vtable_t *decoder_rs41_vtable(void);

/* Diagnostic: returns the last ECEF XYZ (cm) read by the parser, plus the
   age in ms since that read. age_ms_ago == UINT32_MAX means no clean parse
   has happened since boot. */
void decoder_rs41_last_ecef(int32_t *x, int32_t *y, int32_t *z, uint32_t *age_ms_ago);

#ifdef __cplusplus
}
#endif
