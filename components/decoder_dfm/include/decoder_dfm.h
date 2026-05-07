#pragma once

#include "decoder_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Graw DFM-09/DFM-17 frame decoder skeleton. Returns the decoder
   vtable. Real subframe parsing is a TODO (FSD §11.6 Phase B). */
const decoder_vtable_t *decoder_dfm_vtable(void);

#ifdef __cplusplus
}
#endif
