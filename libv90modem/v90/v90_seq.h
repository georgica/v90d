/* V.90 start-up sequences as bit arrays: Jd, MP, the DIL descriptor (Ja),
 * CP / CPt and E.
 *
 * Conventions shared by every codec here:
 *  - a bit array holds one bit (0 or 1) per uint8_t; index 0 is "bit 0" of
 *    the Recommendation's tables and is the first bit in time;
 *  - multi-bit fields are LSB first (the tables' "LSB:MSB" column);
 *  - xxx_build() writes the sequence from bit 0 (frame sync) through its last
 *    fill bit and returns the number of bits written, or a negative V90_ERR_
 *    code if the struct cannot be encoded;
 *  - xxx_parse() expects bits[0] to be bit 0 of the sequence (use
 *    v90_find_sync() to get there) and `n` bits to be available. It returns
 *    the number of bits the sequence occupies (> 0) or a negative V90_ERR_
 *    code. Reserved bits are ignored on parse and written as 0 on build;
 *  - the CRC is 10.1.2.3.2/V.34 read as reflected CRC-CCITT: polynomial 0x8408,
 *    register preset to 0xFFFF, shifted towards bit 0, no final inversion,
 *    sent bit 0 first; a receiver running information + CRC through the same
 *    register ends at 0. It covers every bit between the frame sync and the
 *    CRC except start bits, so reserved bits and the zero padding of SP / TP
 *    are covered; frame sync, start bits and fill bits are not.
 *
 * None of this is tied to a modulation: the digital modem's receiver (V.34
 * demodulator, differential decoder, GPA descrambler) delivers the bits that
 * the parsers take, and test peers use the builders.
 */
#ifndef V90D_V90_SEQ_H
#define V90D_V90_SEQ_H
#include <stdint.h>

#define V90_ERR_SHORT  (-1)   /* fewer than the needed bits available */
#define V90_ERR_SYNC   (-2)   /* frame sync, a start bit or a fill bit is wrong */
#define V90_ERR_CRC    (-3)   /* CRC mismatch */
#define V90_ERR_RANGE  (-4)   /* a field value cannot be encoded / is not allowed */

/* CRC of n bits (register preset 0xFFFF). v90_crc(info + 16 CRC bits) == 0. */
uint16_t v90_crc(const uint8_t *bits, int n);
/* One step of the same register, for callers that stream. */
uint16_t v90_crc_step(uint16_t crc, int bit);

/* Index of the first frame sync at or after `from`: seventeen 1s followed by
 * the start bit 0, and not preceded by a 1 (so the tail of a longer run of
 * ones, such as E, does not match). At index 0 the preceding bit is unknown
 * and taken as 0. Returns -1 if there is none in bits[from..n-1]. */
int v90_find_sync(const uint8_t *bits, int n, int from);

/* ---- Jd (8.4.2, Table 13): digital modem capabilities ------------------ */

#define V90_JD_BITS      72
#define V90_NUM_RATES    22          /* 28000 .. 56000 bit/s in 8000/6 steps */
/* downstream rate index r (0..21) <-> D = r + 21 data bits per data frame
 * <-> (r + 21) * 8000 / 6 bit/s <-> drn = r + 1 in CP */
#define V90_RATE_BPS(r)  (((r) + 21) * 4000 / 3)

typedef struct {
    uint32_t rates;       /* bit r set: downstream rate index r supported */
    uint8_t  cp16_train;  /* bit 47: 16-point CP/E/SCR during training */
    uint8_t  cp16_reneg;  /* bit 48: 16-point CP/E/SCR during rate renegotiation */
    uint8_t  max_ld;      /* bits 49:50: maximum lookahead, 1..3 */
} v90_jd_t;

int v90_jd_build(const v90_jd_t *jd, uint8_t *bits);              /* 72 */
int v90_jd_parse(const uint8_t *bits, int n, v90_jd_t *jd);

/* ---- MP / MP' (8.6.3, Table 16) ----------------------------------------- */

#define V90_MP0_BITS     86          /* Type 0, through the fill bit 85 */
#define V90_MP1_BITS     188         /* Type 1, through the fill bit 187 */
#define V90_MP_MAX_BITS  V90_MP1_BITS

typedef struct {
    uint8_t  type;        /* bit 18: 0 or 1 (1 carries precoder coefficients) */
    uint8_t  max_rate;    /* bits 24:27: drn, upstream max = drn * 2400; 0 = cleardown */
    uint8_t  trellis;     /* bits 29:30: 0 = 16, 1 = 32, 2 = 64 state */
    uint8_t  nonlinear;   /* bit 31 */
    uint8_t  shaping;     /* bit 32: 0 = minimum, 1 = expanded */
    uint8_t  ack;         /* bit 33: 1 makes this MP' */
    uint16_t rates;       /* bits 36:48: bit 0 = 4800, ..., bit 12 = 33600 */
    int16_t  h[3][2];     /* Type 1 only: h(1..3), [0] real, [1] imaginary */
} v90_mp_t;

/* Builds through the single mandatory fill bit. The further zero fill "to the
 * next multiple of 6 symbols" depends on the mapping parameters and is added
 * by the transmitter (v90_tx). */
int v90_mp_build(const v90_mp_t *mp, uint8_t *bits);              /* 86 or 188 */
int v90_mp_parse(const uint8_t *bits, int n, v90_mp_t *mp);

/* ---- DIL descriptor carried in Ja (8.3.1, Table 12) ---------------------- */

#define V90_DIL_MAX_BITS 2654

typedef struct {
    uint8_t n;            /* N: number of DIL-segments, 0..255 */
    uint8_t lsp, ltp;     /* lengths of SP and TP, 1..128 */
    uint8_t sp[16];       /* sign pattern: bit m is sp[m >> 3] >> (m & 7); 1 = positive */
    uint8_t tp[16];       /* training pattern: 1 = training symbol, 0 = REFc */
    uint8_t h[8];         /* H1..H8: segment length for Uchord c is (Hc + 1) * 6 */
    uint8_t ref[8];       /* REF1..REF8: reference Ucode per Uchord */
    uint8_t train[255];   /* training Ucode of each DIL-segment */
} v90_dil_t;

int v90_dil_build(const v90_dil_t *d, uint8_t *bits);
int v90_dil_parse(const uint8_t *bits, int n, v90_dil_t *d);

/* ---- CP / CPt (8.5.2, Table 14) ----------------------------------------- */

#define V90_CP_MAX_BITS  1788

typedef struct {
    uint8_t  is_cp;       /* bit 19: 0 = CPt (Phase 4 training), 1 = CP (data mode) */
    uint8_t  drn;         /* bits 20:24: 0 = cleardown, else D = drn + 20 (CP) or drn + 8 (CPt) */
    uint8_t  silence;     /* bit 30: silent period requested (CPs) */
    uint8_t  sr;          /* bits 31:32: Sr */
    uint8_t  ack;         /* bit 33: 1 makes this CP' */
    uint8_t  alaw;        /* bit 35: codec type */
    uint16_t rates;       /* bits 36:48: upstream capability, bit 0 = 4800 */
    uint8_t  ld;          /* bits 49:50: lookahead frames requested */
    uint16_t rms_ratio;   /* bits 52:67: unsigned Q3.13 */
    int8_t   a1, a2, b1, b2;   /* bits 69:101: shape filter, signed Q1.6 */
    uint8_t  idx[6];      /* constellation index of each data frame interval, 0..5 */
    uint8_t  codec_differs;    /* bit 128 */
    /* Constellations 0 .. max(idx): bit u of mask[c] (mask[c][u >> 3] >> (u & 7))
     * set means Ucode u is in constellation c. codec_mask is only carried when
     * codec_differs is set. Entries above max(idx) are zero after a parse and
     * ignored by build. */
    uint8_t  mask[6][16];
    uint8_t  codec_mask[6][16];
} v90_cp_t;

/* number of constellations carried: max(idx) + 1 */
int v90_cp_nconst(const v90_cp_t *cp);
int v90_cp_build(const v90_cp_t *cp, uint8_t *bits);
int v90_cp_parse(const uint8_t *bits, int n, v90_cp_t *cp);

/* ---- E (8.5.3, 10.1.3.2/V.34): 20 binary ones --------------------------- */

#define V90_E_BITS       20

int v90_e_build(uint8_t *bits);                                   /* 20 */
/* 1 if bits[0..19] are all ones. Note the first 17 look like a frame sync;
 * bit 17 tells them apart (0 = start bit of a sequence, 1 = E). */
int v90_e_match(const uint8_t *bits, int n);

#endif
