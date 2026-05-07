/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reed-Solomon RS(255,231) decoder over GF(2^8) for Vaisala RS41 frames.
 * Original implementation. Uses the standard algorithm — Berlekamp-Massey
 * + Chien search + Forney — applied to GF(2^8) with primitive polynomial
 * 0x11D (the same field used by CCSDS RS, Reed-Solomon codes for RAID-6,
 * QR codes, and RS41 frames).
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode an RS(255,231) codeword in place.
 *
 *   r[0..230]   data (or zero-padded data for shortened codes)
 *   r[231..254] parity (24 bytes)
 *
 * On entry r[] is the received codeword (with possible byte errors).
 * On exit:
 *   - returns the number of corrected byte errors (0..12), with r[] fixed
 *   - returns -1 if the codeword is uncorrectable (more than 12 errors,
 *     or the BM/Chien result is inconsistent — r[] is left untouched).
 *
 * Field: GF(2^8) with primitive polynomial 0x11D, primitive element α = 2.
 * Generator polynomial roots: α^0 .. α^23 (b = 0). */
int rs_decode_255_231(uint8_t r[255]);

/* Diagnostic: count how many of the 24 syndromes are nonzero for the given
   codeword. 0 = clean, 1..24 = some error pattern. Doesn't correct anything. */
int rs_count_nonzero_syndromes(const uint8_t r[255]);

#ifdef __cplusplus
}
#endif
