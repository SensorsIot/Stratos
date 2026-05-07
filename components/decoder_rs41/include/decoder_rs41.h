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

/* Diagnostic: number of byte errors the Reed-Solomon decoder corrected in
   the most recent parsed frame. -1 means RS rejected the frame (more than
   12 errors per codeword); 0 means the frame was already clean. */
int decoder_rs41_last_rs_errs(void);

/* Diagnostic: pre-correction nonzero-syndrome counts for the two
   interleaved codewords (range 0..24). 0 = codeword was already clean;
   24 = every syndrome nonzero (often indicates a systematic mismatch in
   how we extract the codeword from the frame, not actual RF errors). */
int decoder_rs41_last_synd1(void);
int decoder_rs41_last_synd2(void);

/* Diagnostic: copy slot-th raw 312-byte post-sync frame buffer (BEFORE
   any bit-reverse / PRBS / ECC). 0 ≤ slot < 4 (RAW_FRAMES). seqno_out is
   the monotonic frame counter at the time this slot was written; -1 if
   never written yet. */
void decoder_rs41_get_raw(int slot, uint8_t out[312], int *seqno_out);

#ifdef __cplusplus
}
#endif
