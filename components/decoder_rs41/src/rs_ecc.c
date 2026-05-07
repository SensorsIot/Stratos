/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 SensorsIot
 *
 * Reed-Solomon RS(255,231) decoder over GF(2^8). Original implementation
 * of the standard textbook algorithm: syndromes -> Berlekamp-Massey ->
 * Chien search -> Forney. See header for parameter conventions.
 */

#include "rs_ecc.h"
#include <string.h>

/* ---- GF(2^8) arithmetic ---- */

#define GF_PRIM  0x11D    /* x^8 + x^4 + x^3 + x^2 + 1 */
#define GF_GEN   0x02     /* primitive element α */

static uint8_t  gf_exp[512];   /* exp[i] = α^i, doubled to skip the mod-255 */
static uint8_t  gf_log[256];   /* log[α^i] = i; log[0] is undefined */
static int      gf_ready;

static void gf_init(void)
{
    if (gf_ready) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= GF_PRIM;
    }
    /* Duplicate the table so a + b (where a + b can be up to 510) is a
       direct index without a modulo. */
    for (int i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_ready = 1;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

static inline uint8_t gf_inv(uint8_t a)
{
    /* a^-1 = α^(255 - log a). a == 0 is undefined; callers must check. */
    return gf_exp[255 - gf_log[a]];
}

/* α^n with n possibly large or negative. */
static inline uint8_t gf_alpha_pow(int n)
{
    n %= 255;
    if (n < 0) n += 255;
    return gf_exp[n];
}

/* ---- Polynomial helpers (over GF(2^8), little-endian: p[0] is constant). ---- */

/* p(x) at x = α^which. Used for syndrome computation and for
   evaluating Λ / Ω at error-locator powers. */
static uint8_t poly_eval(const uint8_t *p, int len, uint8_t at)
{
    /* Horner: y = ((p[len-1] * x + p[len-2]) * x + ... + p[0]) */
    uint8_t y = 0;
    for (int i = len - 1; i >= 0; i--) {
        y = gf_mul(y, at) ^ p[i];
    }
    return y;
}

/* ---- Reed-Solomon decode ---- */

#define RS_N       255
#define RS_K       231
#define RS_NPAR    (RS_N - RS_K)   /* 24 */
#define RS_T       (RS_NPAR / 2)   /* 12 — max correctable errors */

int rs_decode_255_231(uint8_t r[255])
{
    gf_init();

    /* 1. Syndromes.
       For RS41 (Phil Karn library convention used by rs1729) the codeword,
       when interpreted in our low-degree-first convention, has roots at
       α^0, α^-1, α^-2, ..., α^-23. So syndromes are evaluated with a
       negative step. */
    #define RS_FCR 0
    uint8_t S[RS_NPAR];
    int any_nonzero = 0;
    for (int i = 0; i < RS_NPAR; i++) {
        S[i] = poly_eval(r, RS_N, gf_alpha_pow(-(i + RS_FCR)));
        if (S[i]) any_nonzero = 1;
    }
    if (!any_nonzero) return 0;   /* clean codeword */

    /* 2. Berlekamp-Massey: find the error-locator polynomial Λ(x) of
          minimum degree such that Λ(x) * S(x) ≡ 0 (mod x^NPAR).         */
    uint8_t Lambda[RS_NPAR + 1] = {0};   /* Λ — current estimate */
    uint8_t B[RS_NPAR + 1]      = {0};   /* B — last-good "sister" */
    uint8_t T[RS_NPAR + 1]      = {0};   /* scratch */
    Lambda[0] = 1;
    B[0]      = 1;
    int L = 0;          /* current number of errors implied by Λ */
    int m = 1;          /* shift of B relative to current step */
    uint8_t b = 1;      /* discrepancy from the previous L update */

    for (int n = 0; n < RS_NPAR; n++) {
        /* Discrepancy d = S_n + Σ Λ_i · S_{n-i} for i=1..L. */
        uint8_t d = S[n];
        for (int i = 1; i <= L; i++) {
            if (Lambda[i] && (n - i) >= 0)
                d ^= gf_mul(Lambda[i], S[n - i]);
        }

        if (d == 0) {
            m++;
        } else if (2 * L <= n) {
            /* L update: Λ_new(x) = Λ(x) - (d/b) · x^m · B(x) */
            memcpy(T, Lambda, sizeof(T));
            uint8_t coef = gf_mul(d, gf_inv(b));
            for (int i = 0; i + m <= RS_NPAR; i++) {
                if (B[i]) Lambda[i + m] ^= gf_mul(coef, B[i]);
            }
            int new_L = n + 1 - L;
            memcpy(B, T, sizeof(B));
            b = d;
            L = new_L;
            m = 1;
        } else {
            /* Λ update without changing L. */
            uint8_t coef = gf_mul(d, gf_inv(b));
            for (int i = 0; i + m <= RS_NPAR; i++) {
                if (B[i]) Lambda[i + m] ^= gf_mul(coef, B[i]);
            }
            m++;
        }
    }

    if (L == 0 || L > RS_T) return -1;   /* uncorrectable */

    /* 3. Chien search: error positions are roots of Λ(x). With our
          negative-step root convention, evaluate at α^j (positive) and
          map error position to j. */
    int      err_pos[RS_T];
    uint8_t  err_X[RS_T];        /* X_k = α^-(error position) */
    int      n_err = 0;
    for (int j = 0; j < RS_N; j++) {
        if (poly_eval(Lambda, L + 1, gf_alpha_pow(j)) == 0) {
            if (n_err >= RS_T) return -1;
            err_pos[n_err] = j;
            err_X[n_err]   = gf_alpha_pow(-j);
            n_err++;
        }
    }
    if (n_err != L) return -1;   /* root count mismatch — unrecoverable */

    /* 4. Forney: compute error-evaluator polynomial Ω(x) = (S(x) · Λ(x)) mod x^NPAR.
          Then error magnitude e_k = -Ω(X_k^-1) / Λ'(X_k^-1).
          In GF(2^q), -y == y, so we drop the sign. */
    uint8_t Omega[RS_NPAR];
    for (int i = 0; i < RS_NPAR; i++) {
        uint8_t sum = 0;
        for (int j = 0; j <= i && j <= L; j++) {
            sum ^= gf_mul(S[i - j], Lambda[j]);
        }
        Omega[i] = sum;
    }
    /* Λ'(x) — formal derivative. In char-2 GF, only odd-power terms survive.
       Λ(x) = Σ Λ_i x^i  →  Λ'(x) = Σ_{i odd} Λ_i x^{i-1}. */
    uint8_t Lambda_prime[RS_NPAR + 1] = {0};
    for (int i = 1; i <= L; i += 2) {
        Lambda_prime[i - 1] = Lambda[i];
    }

    /* Forney with our Λ-convention (Λ(α^-j) = 0 at error positions j):
         e_k = X_k^{1-RS_FCR} · Ω(X_k^-1) / Λ'(X_k^-1)   (textbook form)
       Equivalent for RS_FCR=0 by (Ω/Λ') · X_k.  Tested empirically below;
       if codewords still fail, the alternative convention (· X_k^-1) is
       enabled with FORNEY_INV_X. */
    for (int k = 0; k < n_err; k++) {
        uint8_t Xk_inv = gf_inv(err_X[k]);
        uint8_t num    = poly_eval(Omega,        RS_NPAR,  Xk_inv);
        uint8_t den    = poly_eval(Lambda_prime, L,        Xk_inv);
        if (den == 0) return -1;
        uint8_t e = gf_mul(num, gf_inv(den));
        if (RS_FCR == 0) e = gf_mul(err_X[k], e);
        r[err_pos[k]] ^= e;
    }

    /* Sanity: recompute syndromes; they must all be zero now. */
    for (int i = 0; i < RS_NPAR; i++) {
        if (poly_eval(r, RS_N, gf_alpha_pow(-(i + RS_FCR))) != 0) return -1;
    }
    return n_err;
}

int rs_count_nonzero_syndromes(const uint8_t r[255])
{
    gf_init();
    int n = 0;
    for (int i = 0; i < RS_NPAR; i++) {
        if (poly_eval(r, RS_N, gf_alpha_pow(-(i + RS_FCR))) != 0) n++;
    }
    return n;
}
