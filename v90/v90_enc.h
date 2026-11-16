/* V.90 5.4: the digital modem's data mode encoder, as building blocks.
 *
 *   D scrambled bits -> bit parser (5.4.2) -> K bits -> modulus encoder (5.4.3)
 *                                                    -> mapper (5.4.4) -> Ucodes U0..U5
 *                                           -> S bits -> sign precoding (5.4.5.1-4)
 *                                                    -> spectral shaper (5.4.5.5-6)
 *                                                    -> sign bits $0..$5
 *
 * The scrambler (5.3) is in v90_scr.h, the mux and the frame buffering the
 * shaper's lookahead needs are in v90_tx.h. Everything here is integer only
 * and keeps its state in caller-owned structs.
 *
 * Spec readings chosen here (see also the list at the top of v90_tx.h):
 *  - 5.4.5.2: "t_j(k) = p'_j(k) XOR t_(j-1)(k)" is read literally: the second
 *    differential encoder runs on the initial sign assignment t, before the
 *    shaper. Reading t_(j-1) as the shaped sign gives the same set of allowed
 *    sign sequences (only the bookkeeping of the trellis state differs), and
 *    the same receiver decodes both.
 *  - Figure 2: rule A keeps state 0, B goes 0 -> 1, C goes 1 -> 0, D keeps
 *    state 1. With "even" inversion e and "odd" inversion o of a shaping frame
 *    (A: e=o=0, B: e=o=1, C: e=1,o=0, D: e=0,o=1) that is: state = e XOR o,
 *    next state = o.
 *  - 5.4.5.6: the metric filter runs in fixed point, state in Q6 (the
 *    coefficients' own precision), products rounded to nearest, states
 *    saturated at +/-2^29. The filter may have poles on the unit circle
 *    (a = +/-1), where its state is only bounded by the shaper's own success;
 *    saturation changes which rule is preferred, never whether the receiver
 *    can decode. Ties between two rule sequences are resolved towards the
 *    rule that does not invert the odd-numbered bits (A in state 0, C in
 *    state 1), so an all-zero filter leaves the initial signs untouched.
 *  - 5.4.5.5 "for frames j through j+ld": when fewer than ld further shaping
 *    frames are known (end of a signal that is followed by a reset), the
 *    search simply uses those available.
 */
#ifndef V90D_V90_ENC_H
#define V90D_V90_ENC_H
#include <stdint.h>
#include "v90_seq.h"

/* ---- mapping parameters (5.4.1) ------------------------------------------ */

typedef struct {
    uint8_t K;            /* modulus encoder input bits per data frame */
    uint8_t Sr;           /* sign bits used as shaping redundancy, 0..3 */
    uint8_t ld;           /* shaper lookahead depth, 0..3 */
    int8_t  a1, a2, b1, b2;   /* shape filter parameters, signed Q1.6 */
    uint8_t M[6];         /* constellation size per data frame interval */
    uint8_t C[6][128];    /* Ucodes per interval, label order: C[i][0] is the largest */
} v90_map_t;

/* data bits per data frame: D = K + S, S = 6 - Sr */
#define V90_MAP_S(m)  (6 - (m)->Sr)
#define V90_MAP_D(m)  ((m)->K + 6 - (m)->Sr)
#define V90_MAX_D     48  /* bound for bit buffers; the tables allow at most 42 */

/* Load the constellation of data frame interval i from a 128-bit Ucode mask
 * (bit u = mask[u >> 3] >> (u & 7)), labelling in descending Ucode order. */
void v90_map_set_points(v90_map_t *m, int i, const uint8_t mask[16]);

/* 0 if the parameters are usable by the encoder: Sr, ld <= 3, 1 <= Mi <= 128,
 * points strictly descending Ucodes < 128, and 2^K <= M0*...*M5; else
 * V90_ERR_RANGE. Does not apply the K/S limits of Tables 2 and 17. */
int v90_map_check(const v90_map_t *m);

/* Derive the mapping parameters from a parsed CP or CPt:
 *   D = drn + 20 (CP) or drn + 8 (CPt),  S = 6 - Sr,  K = D - S,
 * constellation of interval i = mask[idx[i]].
 * Returns 0 on success, V90_MAP_CLEARDOWN if drn = 0 (m is not written), or
 * V90_ERR_RANGE if the sequence asks for something V.90 does not allow:
 * drn > 22, K outside Table 2 (CP: 15..39) or Table 17 (CPt: 6..24), an empty
 * constellation, or 2^K > M0*...*M5. ld is copied as sent; whether it is
 * within what Jd offered is for the caller to check. The CP's codec type,
 * rate mask, RMS ratio and codec constellations are not used here. */
#define V90_MAP_CLEARDOWN 1
int v90_map_from_cp(v90_map_t *m, const v90_cp_t *cp);

/* ---- modulus encoder and mapper (5.4.3, 5.4.4) --------------------------- */

/* K bits b[0..K-1] (b0 first) -> K0..K5. Multi-precision short division, no
 * 64-bit divide. */
void v90_modulus_encode(const v90_map_t *m, const uint8_t *b, uint8_t k[6]);

/* ---- sign precoding (5.4.5.1 to 5.4.5.4, Tables 3 and 4) ----------------- */

typedef struct {
    uint8_t podd;         /* last differentially encoded odd bit p' */
    uint8_t tprev[6];     /* t of the previous shaping frame; [0] doubles as
                             "$5 of the previous data frame" when Sr = 0 */
} v90_diff_t;

void v90_diff_init(v90_diff_t *d);       /* "differential encoder initialized to zero" */

/* S = 6 - Sr sign input bits s[0..S-1] -> six bits t[0..5] in data frame
 * interval order. For Sr = 0 these are the final sign bits $0..$5; for
 * Sr > 0 they are the initial assignment t the spectral shaper works on. */
void v90_sign_precode(v90_diff_t *d, int Sr, const uint8_t *s, uint8_t t[6]);

/* Whole front half of the encoder for one data frame: D bits d[0..D-1]
 * (already scrambled) -> Ucodes u[0..5] and sign bits t[0..5] as above. */
void v90_frame_encode(const v90_map_t *m, v90_diff_t *d, const uint8_t *bits,
                      uint8_t u[6], uint8_t t[6]);

/* ---- spectral shaper (5.4.5.5, 5.4.5.6) ---------------------------------- */

typedef struct {
    int32_t x1;           /* x[n-1], linear units */
    int32_t y1;           /* y[n-1], Q6 */
    int32_t v1;           /* v[n-1], Q6 */
    uint8_t q;            /* trellis state Q */
} v90_shaper_t;

void v90_shaper_init(v90_shaper_t *sh);  /* filter memory and state zero */

/* One step of the metric filter for input x (signed linear value); returns
 * v[n] in Q6. Exposed for tests. */
int32_t v90_shaper_step(v90_shaper_t *sh, const v90_map_t *m, int32_t x);

/* Shape one shaping frame (6 / Sr symbols; Sr must be 1..3).
 *   lin[]   linear magnitudes (>= 0) of the symbols of `nframes` consecutive
 *           shaping frames, the one to shape first
 *   t[]     their initial sign bits
 *   nframes how many shaping frames lin/t hold (>= 1); min(nframes, ld + 1)
 *           are searched
 *   sign[]  receives the final sign bits of the first shaping frame
 * Advances the filter memory and trellis state over that frame and returns
 * the rule applied: 0 = A, 1 = B, 2 = C, 3 = D. */
int v90_shape(v90_shaper_t *sh, const v90_map_t *m, const int32_t *lin,
              const uint8_t *t, int nframes, uint8_t *sign);

#endif
