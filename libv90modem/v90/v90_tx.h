/* V.90 digital modem transmitter, Phase 3 onwards: one G.711 octet per call.
 *
 * The object generates every signal the digital modem sends from Sd to data
 * mode (V.90 8.4, 8.6, 5.4) as PCM codewords, and keeps the 6-symbol data
 * frame alignment that starts with the first symbol of Sd (8.4.4). It does no
 * signal processing and has no notion of time other than counting symbols; a
 * Phase 3/4 state machine drives it:
 *
 *     v90tx_init(&tx, alaw, uinfo);          // UINFO from INFO1a
 *     v90tx_set_jd(&tx, &caps);
 *     v90tx_load_dil(&tx, &dil);             // parsed from Ja
 *     v90tx_select(&tx, V90TX_SD);
 *     for each 8 kHz sample:
 *         octet = v90tx_tick(&tx);
 *         ... look at v90tx_signal / v90tx_symbols / v90tx_units and at the
 *             receiver, and v90tx_select() the next signal when it is time ...
 *
 * Signals, their unit (the granule in which they are counted and at whose
 * boundary a change takes effect) and what follows when nothing was selected:
 *
 *   signal        unit                 length     then (default successor)
 *   SILENCE       6 symbols            until told
 *   SD            6 symbols            64 units   SD_BAR
 *   SD_BAR        6 symbols            8 units    TRN1D
 *   TRN1D         6 symbols            until told
 *   JD            one Jd, 72 symbols   until told
 *   JD_PRIME      12 symbols           1 unit     DIL if N > 0, else RI
 *   DIL           one DIL-segment      until told
 *   RI, RT, RD    6 symbols            until told
 *   RI_BAR        6 symbols            4 units    TRN2D
 *   RD_BAR        6 symbols            4 units    TRN2D
 *   RT_BAR        6 symbols            4 units    MP
 *   TRN2D         one data frame       until told
 *   MP, MP_ACK    one MP sequence      until told
 *   ED            one data frame       2 units    B1D
 *   B1D           one data frame       48 units   DATA
 *   DATA          one data frame       until told
 *
 * v90tx_select(sig) names the signal that follows the current one. For an
 * "until told" signal it takes effect at the next unit boundary (so the
 * current Jd, MP or DIL-segment is completed, as 9.3.1.5, 9.3.1.6 and 9.4.1.3
 * require); for a fixed-length signal it replaces the default successor and
 * takes effect when the fixed length is complete. One selection can be
 * outstanding: a further call replaces any earlier selection whose first
 * symbol has not been sent yet (so wait for v90tx_signal() to report MP_ACK
 * and at least one unit of it before selecting ED, for instance). If a
 * default successor lacks its parameters (no CP loaded, no getbit) SILENCE is
 * sent. Selecting the signal already running restarts it.
 *
 * Before the first selection after v90tx_init() the transmitter sends
 * silence and has no frame alignment: the first selected signal starts on the
 * very next tick, and its first symbol defines data frame interval 0. From
 * then on everything stays on that 6-symbol grid, SILENCE included.
 *
 * Lookahead and latency. With spectral shaping and ld > 0 the encoder must
 * know the next ld shaping frames, so data frames are built (scrambled,
 * mapped) up to 3 data frames before they are sent, and the getbit callback
 * is called that much early. Nothing is delayed: the first data frame of
 * TRN2d / B1d is the first one sent. v90tx_select() discards frames that were
 * built ahead but not yet started and re-builds from the earliest unit
 * boundary, so a change still takes effect at the first boundary after the
 * symbol being sent; the exception is DATA, whose built-ahead frames hold
 * user bits and are always sent (a change away from DATA can take up to 3
 * data frames longer). All queries below describe what has been SENT.
 *
 * Cost. Without shaping a symbol is a table lookup. With shaping the search
 * runs the metric filter over 2 + 4 + ... + 2^(ld+1) shaping frames per
 * shaping frame sent, i.e. about 3, 7, 15 and 31 filter steps per symbol for
 * ld = 0..3 (each step four 32x32->64 multiplies). ld is chosen by the
 * analogue modem up to the maximum announced in Jd, so v90_jd_t.max_ld is the
 * knob that bounds it (1 is the least V.90 lets a digital modem announce).
 *
 * Parameters (Jd, MP, DIL descriptor, maps) are copied in. Do not change the
 * parameters of a signal while it is being sent or is the outstanding
 * selection, except MP: v90tx_set_mp() may be called at any time and applies
 * from the next MP sequence. No allocation, no globals: any number of
 * instances may run side by side.
 *
 * ---------------------------------------------------------------------------
 * Spec ambiguities and the readings chosen (V.90 09/98)
 *
 *  1. Overbars are missing from the text layer of the PDF. Read as: Sd is 64
 *     repetitions of {+W,+0,+W,-W,-0,-W}, Sd-bar 8 repetitions of the inverse
 *     (8.4.4); R is the +++--- pattern repeated, R-bar 4 repetitions of
 *     ---+++ (8.6.4); "Ri for 24T" in 9.4.1.2 is Ri-bar.
 *  2. "+0" and "-0" in Sd are Ucode 0 with positive and negative polarity
 *     bit (mu-law 0xFF / 0x7F, A-law 0xD5 / 0x55).
 *  3. 8.4.2 "scrambled and differentially encoded" gives no rule. Read as
 *     sign[n] = scrambled_bit[n] XOR sign[n-1], seeded with the sign of the
 *     last TRN1d symbol; Jd' continues from the last Jd symbol. TRN1d itself
 *     is not differentially encoded (8.4.5). The scrambler is zeroed before
 *     TRN1d only and runs on through Jd and Jd'.
 *  4. Sd needs Ucode 16 + UINFO, so UINFO > 111 cannot be honoured;
 *     v90tx_init() refuses it. (The Recommendation only says UINFO > 66.)
 *  5. 8.4.1 DIL: Hc and REFc are selected by the Uchord of the segment's
 *     training Ucode. SP and TP restart at each DIL-segment and each repeats
 *     with its own period inside it. N = 0 means there is no DIL at all.
 *  6. 8.6.3 MP "transmitted using the constellation parameters used to send
 *     TRN2d": read as MP bits going through the scrambler (which runs on from
 *     TRN2d) and the 5.4 encoder like data. "Fill ... to the next multiple of
 *     6 symbols": zeros up to a whole number of data frames, so every MP
 *     starts on a data frame boundary. Ed (8.6.2) is two data frames of
 *     zeros into the same scrambler and encoder, with no reset.
 *  7. 8.6.1 / 8.6.5 "the first data frame ... shall be identical for all
 *     values of ld": read as: no filler frame may be inserted to prime the
 *     lookahead and the magnitudes do not depend on ld. The shaper starts
 *     from zero state; with ld > 0 the signs it picks for the first frame can
 *     depend on the frames it looks ahead to.
 *  8. B1d is followed by data without re-initialising scrambler, differential
 *     encoder or filter (only "prior to transmitting B1d", 8.6.1).
 *  9. 5.4.5: see v90_enc.h for the differential encoder, trellis, fixed-point
 *     metric, tie and partial-lookahead readings.
 * 10. 5.4.5.6 "linear value corresponding to PCM codes being transmitted":
 *     the Table 1 column of the law this transmitter sends (v90tx_init), not
 *     the codec type bit of CP.
 * 11. 9.6.1.2.5 silence is "PCM codewords with magnitudes represented by
 *     Ucode 0"; the sign is not given. Positive is sent.
 * 12. 8.6 "During rate renegotiation, TRN2d, MP and Ed use the spectral
 *     shaping parameters as used in the preceding data mode along with the K
 *     previously derived from CPt": not automated here. The caller can build
 *     such a mixed map and install it with v90tx_set_map().
 * 13. CRC (10.1.2.3.2/V.34), CP / DIL descriptor layout details: v90_seq.h.
 */
#ifndef V90D_V90_TX_H
#define V90D_V90_TX_H
#include <stdint.h>
#include "v90_scr.h"
#include "v90_seq.h"
#include "v90_enc.h"

typedef enum {
    V90TX_NONE = -1,
    V90TX_SILENCE = 0,    /* Ucode 0, positive */
    V90TX_SD,             /* 8.4.4 */
    V90TX_SD_BAR,
    V90TX_TRN1D,          /* 8.4.5 */
    V90TX_JD,             /* 8.4.2, needs v90tx_set_jd */
    V90TX_JD_PRIME,       /* 8.4.3 */
    V90TX_DIL,            /* 8.4.1, needs v90tx_load_dil with N > 0 */
    V90TX_RI,             /* 8.6.4, UINFO in every interval */
    V90TX_RI_BAR,
    V90TX_RT,             /* 8.6.4, largest point of each training constellation (CPt) */
    V90TX_RT_BAR,
    V90TX_RD,             /* 8.6.4, largest point of each data constellation (CP) */
    V90TX_RD_BAR,
    V90TX_TRN2D,          /* 8.6.5, training map */
    V90TX_MP,             /* 8.6.3, acknowledge bit 0, training map, needs v90tx_set_mp */
    V90TX_MP_ACK,         /* MP', acknowledge bit 1 */
    V90TX_ED,             /* 8.6.2, training map */
    V90TX_B1D,            /* 8.6.1, data map */
    V90TX_DATA,           /* 5.4, data map, needs v90tx_set_getbit */
    /* V.92 (the list of V.92 signals is below the V.90 table) */
    V90TX_JP,             /* 8.6.3/V.92, needs v90tx_set_xseq(V90TX_XSEQ_JP) */
    V90TX_JP_PRIME,       /* 8.6.4/V.92 */
    V90TX_SCR,            /* 8.6.6/V.92 */
    V90TX_SUVD,           /* 8.8.5/V.92, training map, needs V90TX_XSEQ_SUVD */
    V90TX_SUVD_ACK,       /* SUVd' */
    V90TX_CPD,            /* 8.8.3/V.92, training map, needs V90TX_XSEQ_CPD */
    V90TX_CPD_ACK,        /* CPd' */
    V90TX_RF,             /* 8.8.4/V.92, largest point of each data constellation */
    V90TX_RF_BAR,
    V90TX_NSIG
} v90tx_sig_t;

/* Supplies the next data bit (0 or 1), first in time first. */
typedef int (*v90tx_getbit_fn)(void *ctx);

#define V90TX_MAP_TRAIN 0     /* from CPt: TRN2d, MP, Ed, Rt */
#define V90TX_MAP_DATA  1     /* from CP: B1d, data, Rd */

/* ---- V.92 ------------------------------------------------------------------
 * A V.92 digital modem sends the V.90 signals above and these (V.92 8.6, 8.8):
 *
 *   signal        unit                 length     then (default successor)
 *   JP            one Jp, 72 symbols   until told
 *   JP_PRIME      12 symbols           1 unit     DIL if N > 0, else SCR
 *   SCR           6 symbols            until told
 *   SUVD, _ACK    one SUVd sequence    until told
 *   CPD, CPD_ACK  one CPd sequence     until told
 *   RF            12 symbols           until told
 *   RF_BAR        12 symbols           2 units    SILENCE
 *
 * Jd of V.92 (Table 21/V.92) is the Jd of V.90 with both cp16 flags zero.
 * Jp travels as Jd does (scrambled, differentially encoded signs of UINFO,
 * the differential encoder running on from Jd). SCR is UINFO with the
 * scrambler output for ones as the sign, the scrambler running on (8.6.6).
 * SUVd and CPd travel as MP does: scrambled, through the training map,
 * zero filled to whole data frames ("to the next multiple of 6 symbols").
 * Ed, B1d, TRN2d, Ri, Rd and Rt are the V.90 signals.
 *
 * The bits of Jp, SUVd, SUVd', CPd and CPd' are built by the caller
 * (../v92/v92_seq.h) and handed over packed, bit i in packed[i >> 3] bit
 * (i & 7) (v90_pack_bits). The transmitter keeps the pointer: the buffer
 * must stay valid and unchanged while its signal is selected or being sent.
 *
 * Readings: Rf "is transmitted by repeating the 12-symbol sequence" with the
 * sign pattern + + - - (three times); the PCM codeword of each symbol is the
 * largest of the data constellation of its data frame interval (i mod 6).
 * Rf-bar is two repetitions of the inverse; what follows it in a fast
 * parameter exchange (9.9) is the owner's business, by default silence. */
#define V90TX_XSEQ_JP        0
#define V90TX_XSEQ_SUVD      1
#define V90TX_XSEQ_SUVD_ACK  2
#define V90TX_XSEQ_CPD       3
#define V90TX_XSEQ_CPD_ACK   4
#define V90TX_NXSEQ          5

/* ---- internals (the struct is public only so it can be embedded) --------- */

#define V90TX_QLEN 4          /* current data frame + up to 3 of lookahead */

typedef struct {              /* encoder front: all that decides future frames */
    int8_t     sig;
    uint8_t    dsign;         /* last sign of TRN1d / Jd / Jd' */
    uint16_t   fr;            /* data frame index inside the current unit */
    uint16_t   nfr;           /* data frames in the current unit */
    uint16_t   seg;           /* DIL-segment index */
    uint32_t   units;         /* units completed in this signal */
    v90_scr_t  scr;
    v90_diff_t diff;
} v90tx_front_t;

typedef struct {
    uint8_t u[6];             /* Ucodes */
    uint8_t t[6];             /* sign bits: final, or initial if shaped */
    int8_t  sig;
    uint8_t flags;
    v90tx_front_t snap;       /* front state before this frame was built */
} v90tx_frame_t;

typedef struct {
    uint8_t alaw, uinfo;
    uint8_t aligned;          /* frame grid established */
    uint8_t phase;            /* data frame interval of the next symbol */
    uint8_t have;             /* parameter presence bits */
    int8_t  pending;          /* outstanding selection or V90TX_NONE */
    int8_t  cur_sig;          /* signal of the last symbol sent */
    uint32_t nsym, nunits;    /* sent in cur_sig */
    uint8_t out[6];           /* octets of the data frame being sent */
    uint8_t qn;               /* frames queued, q[0] is the one being sent */
    v90tx_frame_t q[V90TX_QLEN];
    v90tx_front_t front;
    v90_shaper_t sh;
    uint8_t seq[(V90_MP_MAX_BITS + 7) / 8];   /* packed bits of the Jd / MP unit in progress */
    uint16_t seqlen;
    const uint8_t *seqp;      /* the unit's bits: seq, or one of xseq */
    const uint8_t *xseq[V90TX_NXSEQ]; uint16_t xlen[V90TX_NXSEQ];
    v90tx_getbit_fn getbit;
    void *getbit_ctx;
    v90_jd_t jd;
    v90_mp_t mp;
    v90_dil_t dil;
    v90_map_t map[2];
} v90tx_t;

/* ---- API ------------------------------------------------------------------ */

/* Reset everything: silence, no alignment, no parameters. alaw selects the
 * law of the octets (0 = mu-law). uinfo is UINFO, bits 25:31 of INFO1a.
 * Returns 0, or -1 if uinfo > 111 (the object is still initialised, with
 * uinfo = 0). */
int v90tx_init(v90tx_t *t, int alaw, int uinfo);

/* Source of user data for V90TX_DATA. */
void v90tx_set_getbit(v90tx_t *t, v90tx_getbit_fn fn, void *ctx);

/* Capabilities announced in Jd. Returns 0 or V90_ERR_RANGE. */
int v90tx_set_jd(v90tx_t *t, const v90_jd_t *jd);

/* Contents of MP and MP' (the ack field of *mp is ignored: V90TX_MP sends 0,
 * V90TX_MP_ACK sends 1). Applies from the next MP sequence that starts.
 * Returns 0 or V90_ERR_RANGE. */
int v90tx_set_mp(v90tx_t *t, const v90_mp_t *mp);

/* DIL descriptor received in Ja. Returns 0 or V90_ERR_RANGE. */
int v90tx_load_dil(v90tx_t *t, const v90_dil_t *d);

/* Mapping parameters from a parsed CPt (training map) or CP (data map),
 * chosen by cp->is_cp. Returns 0, V90_MAP_CLEARDOWN (nothing loaded), or
 * V90_ERR_RANGE (nothing loaded); see v90_map_from_cp(). The caller decides
 * what a CP with the silence bit (CPs) means; its parameters must not be
 * loaded (8.5.2). */
int v90tx_load_cp(v90tx_t *t, const v90_cp_t *cp);

/* Install mapping parameters directly (which = V90TX_MAP_TRAIN / _DATA).
 * Returns 0 or V90_ERR_RANGE (v90_map_check). */
int v90tx_set_map(v90tx_t *t, int which, const v90_map_t *m);

/* V.92: the bits of Jp, SUVd, SUVd', CPd or CPd' (which = V90TX_XSEQ_x),
 * packed, nbits of them (1..65535). See the V.92 notes above. Returns 0 or
 * V90_ERR_RANGE. */
int v90tx_set_xseq(v90tx_t *t, int which, const uint8_t *packed, int nbits);
/* bits[0..n-1] (one per byte) into packed[0..(n+7)/8-1] */
void v90_pack_bits(const uint8_t *bits, int n, uint8_t *packed);

/* Choose the signal that follows the current one (see the table above).
 * Returns 0, or -1 if the signal's parameters are missing (nothing changes). */
int v90tx_select(v90tx_t *t, v90tx_sig_t sig);

/* The next PCM octet. Call once per 8 kHz sample. */
uint8_t v90tx_tick(v90tx_t *t);

/* Signal the most recently sent symbol belongs to (SILENCE before any). */
v90tx_sig_t v90tx_signal(const v90tx_t *t);
/* Symbols of that signal sent so far, including the last one. */
uint32_t v90tx_symbols(const v90tx_t *t);
/* Complete units of that signal sent so far (repetitions, Jd or MP
 * sequences, DIL-segments, data frames: see the table above). */
uint32_t v90tx_units(const v90tx_t *t);
/* Selection not yet taken up by the encoder, or V90TX_NONE. */
v90tx_sig_t v90tx_pending(const v90tx_t *t);
/* Data frame interval (0..5) the next tick will send. */
int v90tx_phase(const v90tx_t *t);

#endif
