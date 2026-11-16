/* V.92 forms of the DPSK and V.21 information sequences:
 *
 *   Phase 2 (8.4.1):   INFO0d (Table 15), INFO0a (Table 16), INFO1d
 *                      (Table 17), INFO1a (Table 18 when PCM upstream is
 *                      selected, Table 19 and Tables 10 and 11 of V.90
 *                      otherwise)
 *   modem-on-hold:     the MH sequences (8.9.2, Tables 32 and 33)
 *   short Phase 1:     QC1a, QCA1a, QC1d, QCA1d (8.2, 8.3; Tables 2, 4, 11, 13)
 *
 * Conventions as in v92_seq.h: one bit (0 or 1) per uint8_t, index 0 is
 * "bit 0" of the tables and first in time, integer fields LSB first.
 * xxx_build() writes the whole sequence from bit 0 (the leading fill) and
 * returns its length in bits or a negative V92_ERR_ code. xxx_parse()
 * expects bits[0] to be bit 0 and returns the length (> 0) or a negative
 * V92_ERR_ code.
 *
 * INFO and MH frames: fill 1111, frame sync 01110010 (left-most bit first),
 * information bits, CRC, fill 1111. The CRC is the one of ../v90/v90_seq.h
 * and covers the information bits (bit 12 up to the CRC), the reading
 * ../modem/info.c uses for the V.90 forms. The parsers do not look at the
 * leading fill (bits 0:3) nor at the trailing one; they check the frame
 * sync and the CRC and need n to reach the last CRC bit. A receiver that
 * has found the sync itself and holds the bits from bit 12 on passes that
 * pointer minus V92_INFO_HDR_BITS, provided the 8 sync bits are in its
 * buffer.
 *
 * Reserved bits are written as the tables say and ignored on parse.
 */
#ifndef V90D_V92_INFO_H
#define V90D_V92_INFO_H
#include "v92.h"

#define V92_INFO_HDR_BITS   12      /* fill + frame sync */
#define V92_INFO0D_BITS     62
#define V92_INFO0A_BITS     49
#define V92_INFO1D_BITS     109
#define V92_INFO1A_BITS     70
#define V92_MH_BITS         40
#define V92_INFO_MAX_BITS   109

/* Index of bit 0 of the first frame at or after `from`: the place 4 bits
 * before a frame sync 01110010. -1 if there is none in bits[from..n-1]. */
int v92_info_find(const uint8_t *bits, int n, int from);

/* ---- INFO0d (Table 15) and INFO0a (Table 16) ---------------------------- */

typedef struct {
    uint8_t sr2743, sr2800, sr3429;     /* bits 12, 13, 14 */
    uint8_t lo3000, hi3000, lo3200, hi3200;   /* bits 15..18 */
    uint8_t allow3429;                  /* bit 19: 0 = 3429 disallowed */
    uint8_t pwr_reduce;                 /* bit 20 */
    uint8_t max_sr_diff;                /* bits 21:23, 0..5 */
    uint8_t cme, c1664;                 /* bits 24, 25 */
    uint8_t short_p2;                   /* short Phase 2 requested: bit 26 of
                                           INFO0d, bit 27 of INFO0a */
    uint8_t v92;                        /* V.92 capability: bit 27 of INFO0d,
                                           bit 26 of INFO0a */
    uint8_t ack;                        /* bit 28 */
    /* INFO0d only */
    uint8_t tx_power;                   /* bits 29:32: 0..15 = -6 .. -21 dBm0 */
    uint8_t max_tx_power;               /* bits 33:37: 0..31 = -0.5 .. -16 dBm0 */
    uint8_t pwr_at_codec, alaw, up3429; /* bits 38, 39, 40 */
} v92_info0_t;

int v92_info0d_build(const v92_info0_t *i, uint8_t *bits);              /* 62 */
int v92_info0d_parse(const uint8_t *bits, int n, v92_info0_t *i);
int v92_info0a_build(const v92_info0_t *i, uint8_t *bits);              /* 49 */
int v92_info0a_parse(const uint8_t *bits, int n, v92_info0_t *i);

/* ---- INFO1d (Table 17) -------------------------------------------------- */

typedef struct { uint8_t hi_carrier, preemph, max_rate; } v92_probe_t;

typedef struct {
    uint8_t min_pwr_red, add_pwr_red;   /* bits 12:14, 15:17: dB, 0..7 */
    uint8_t md_len;                     /* bits 18:24: x 35 ms */
    v92_probe_t sr[6];                  /* 2400 2743 2800 3000 3200 3429; the
                                           3429 entry has no carrier bit */
    uint8_t pcm_up;                     /* bit 70: 0 = the channel does not
                                           support PCM upstream */
    int16_t freq_off;                   /* bits 79:88: 0.02 Hz units, -512 = ignore */
} v92_info1d_t;

int v92_info1d_build(const v92_info1d_t *i, uint8_t *bits);             /* 109 */
int v92_info1d_parse(const uint8_t *bits, int n, v92_info1d_t *i);

/* ---- INFO1a -------------------------------------------------------------- */

/* What the analogue modem selected, from bits 34:36 and 37:39:
 *   34:36 = 6 and 37:39 = 6        PCM upstream (Table 18/V.92)
 *   34:36 = 0..5 and 37:39 = 6     V.34 upstream, PCM downstream: V.90
 *                                  (Table 10/V.90, and Table 19/V.92 in
 *                                  short Phase 2, which differ in bit 33 only)
 *   37:39 = 0..5                   V.34 both ways (Table 11/V.90) */
enum { V92_INFO1A_PCM = 0, V92_INFO1A_V90 = 1, V92_INFO1A_V34 = 2 };

typedef struct {
    uint8_t kind;           /* V92_INFO1A_x */
    uint8_t md_len;         /* bits 18:24: MD length; PCM: x 276 symbols
                               (34.5 ms), otherwise x 35 ms */
    uint8_t uinfo;          /* PCM, V90: bits 25:31, Ucode of the 2-point train */
    /* PCM only */
    uint8_t sections;       /* bits 12:13: bit 0 set = z1 supported, bit 1
                               set = p2 supported; p1 and z2 always are */
    uint8_t ltot_code;      /* bits 14:15: Ltot = 192 + 64 * code */
    uint8_t lmax_code;      /* bits 16:17: Lmax = 128 + 64 * code */
    /* V90 and V34 */
    uint8_t up_sr;          /* bits 34:36: upstream symbol rate index, 0 = 2400
                               .. 5 = 3429 (V90: 3..5) */
    uint8_t hi_carrier;     /* V90: bit 33, meaningful in short Phase 2 only
                               (Table 19); V34: bit 25 */
    int16_t freq_off;       /* bits 40:49 */
    /* V34 only */
    uint8_t down_sr;        /* bits 37:39 */
    uint8_t min_pwr_red, add_pwr_red, preemph, max_rate;
} v92_info1a_t;

#define V92_INFO1A_LTOT(code)  (192 + 64 * (code))
#define V92_INFO1A_LMAX(code)  (128 + 64 * (code))

/* V92_ERR_RANGE on build if a field does not fit; for kind PCM bits 40:49
 * are written as ones (the Note of Table 18). On parse V92_ERR_RANGE means
 * a symbol rate code of 7, or 34:36 = 6 without 37:39 = 6. */
int v92_info1a_build(const v92_info1a_t *i, uint8_t *bits);             /* 70 */
int v92_info1a_parse(const uint8_t *bits, int n, v92_info1a_t *i);

/* 1 if the precoder and prefilter lengths of f respect what a PCM upstream
 * INFO1a announced (8.8.3): no z1 or p2 unless supported, each section at
 * most Lmax, all together at most Ltot, and LZ2 >= 1. Else 0. */
int v92_filt_allowed(const v92_filt_t *f, const v92_info1a_t *a);

/* ---- MH sequences (8.9.2) ------------------------------------------------ */

/* Signal indication, bits 12:15, as the table prints the pattern (left-most
 * bit first in time = most significant here). */
enum {
    V92_MH_REQ  = 0x3,      /* 0011 request the remote modem to go on hold */
    V92_MH_ACK  = 0x5,      /* 0101 agree; info = T1 code */
    V92_MH_NACK = 0x7,      /* 0111 deny */
    V92_MH_CLRD = 0x9,      /* 1001 request cleardown; info = reason */
    V92_MH_CDA  = 0xB,      /* 1011 acknowledge cleardown */
    V92_MH_FRR  = 0xD       /* 1101 request fast reconnect */
};
enum { V92_MH_CLRD_INCOMING = 0x5, V92_MH_CLRD_OUTGOING = 0x6, V92_MH_CLRD_OTHER = 0xA };

typedef struct {
    uint8_t sig;            /* V92_MH_x */
    uint8_t info;           /* bits 16:19 printed the same way. MHack: T1 code
                               of Table 33; MHclrd: V92_MH_CLRD_x; the others
                               repeat sig (build writes sig, whatever is here) */
} v92_mh_t;

/* V92_ERR_RANGE: sig is not one of the six. On parse also: a signal that
 * must repeat itself in bits 16:19 does not. */
int v92_mh_build(const v92_mh_t *m, uint8_t *bits);                     /* 40 */
int v92_mh_parse(const uint8_t *bits, int n, v92_mh_t *m);

/* Table 33: on-hold timeout in seconds for a T1 code, -1 = no limit (1101),
 * 0 = reserved code. */
int v92_mh_t1_seconds(int code);

/* ---- short Phase 1: QC1a, QCA1a, QC1d, QCA1d ----------------------------- */

#define V92_QC_BITS   60
#define V92_QCA_BITS  70

typedef struct {
    uint8_t digital;        /* bit 21: 0 = from the analogue modem (QC1a,
                               QCA1a), 1 = from the digital modem */
    uint8_t qca;            /* bit 22: 0 = QC, 1 = QCA */
    uint8_t lapm;           /* bit 23: P */
    uint8_t code;           /* analogue: WXYZ of Table 2 as printed (W most
                               significant), 15 = cleardown from on-hold;
                               digital: LM of Table 11 (L most significant) */
} v92_qc_t;

int v92_qc_build(const v92_qc_t *q, uint8_t *bits);                     /* 60 or 70 */
/* bits[0] is bit 0 (the first of the ten ones). Both copies of the 10-bit
 * frame must be well formed and equal, else V92_ERR_SYNC. */
int v92_qc_parse(const uint8_t *bits, int n, v92_qc_t *q);

/* Table 2: UQTS for a WXYZ code 0..14; 0 for code 15 (cleardown). */
int v92_qc_uqts(int code);
/* Tables 11 and 13: ANSpcm level for an LM code 0..3 in units of 0.5 dBm0:
 * -19 (-9.5 dBm0), -24, -30, -36. */
int v92_qc_anspcm_level(int code);

#endif
