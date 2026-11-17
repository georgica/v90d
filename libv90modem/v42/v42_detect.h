/* V.42 detection phase patterns (ITU-T V.42 clause 7.2.1) and the
 * start-stop character framing they ride on.
 *
 * Before LAPM is up the line carries asynchronous characters: one start bit
 * (0), eight data bits least significant first, one stop bit (1), mark (1)
 * when idle. The originator sends the ODP, the answerer replies with an ADP.
 * The same character framer serves the non-error-corrected fallback mode. */
#ifndef V90D_V42_DETECT_H
#define V90D_V42_DETECT_H
#include <stdint.h>

/* ------------------------------------------------ start-stop characters */
#define V42ASYNC_NONE    (-1)   /* no character completed on this bit */
#define V42ASYNC_FRAMING (-2)   /* stop bit was 0: character discarded */

typedef struct {
    int     state;
    int     nbits;
    uint8_t ch;
    int     ones;      /* mark bits seen since the last stop bit (saturates) */
    int     gap;       /* mark bits between the previous stop bit and the
                          start bit of the character just returned */
} v42async_rx_t;

void v42async_rx_init(v42async_rx_t *r);
/* Returns a character 0..255 when one completes, else V42ASYNC_*. */
int  v42async_rx_bit(v42async_rx_t *r, int bit);

typedef struct {
    uint16_t sr;       /* bits still to send, next bit in bit 0 */
    int      nbits;
} v42async_tx_t;

void v42async_tx_init(v42async_tx_t *t);
int  v42async_tx_idle(const v42async_tx_t *t);
void v42async_tx_char(v42async_tx_t *t, uint8_t c);   /* only when idle */
int  v42async_tx_bit(v42async_tx_t *t);               /* mark when idle */

/* ------------------------------------------------------ pattern sender */
enum {
    V42DET_PAT_ODP = 0,     /* DC1 even parity, DC1 odd parity   (7.2.1.2) */
    V42DET_PAT_ADP_V42,     /* "E", "C": V.42 supported          (Table 3) */
    V42DET_PAT_ADP_NOEC     /* "E", NUL: no error correction desired       */
};

typedef struct {
    uint8_t  ch[2];
    int      gap;           /* mark bits after each stop bit, 8 to 16 */
    int      which;         /* character of the pair being sent */
    int      pos;           /* bit position within character plus gap */
    unsigned patterns;      /* complete patterns (both characters) sent */
} v42det_tx_t;

void v42det_tx_init(v42det_tx_t *t, int pattern, int gap);
int  v42det_tx_bit(v42det_tx_t *t);
/* True when the next bit starts a new pattern (a clean place to stop). */
int  v42det_tx_boundary(const v42det_tx_t *t);

/* ---------------------------------------------------- pattern detector */
enum {
    V42DET_NONE = 0,
    V42DET_ODP,             /* answerer: four DC1s of alternating parity */
    V42DET_ADP_V42,         /* originator: two adjacent "EC" patterns */
    V42DET_ADP_NOEC         /* originator: two adjacent "E NUL" patterns */
};

typedef struct {
    v42async_rx_t rx;
    int     look_for_adp;   /* 1 = originator hunting an ADP, 0 = answerer */
    uint8_t hist[4];        /* last characters, newest in hist[0] */
    int     run;            /* characters in a row with a legal separation */
    int     result;         /* latched V42DET_* */
    int     last_char;      /* character completed by the last bit, or -1 */
} v42det_rx_t;

void v42det_rx_init(v42det_rx_t *d, int look_for_adp);
/* Feed one line bit. Returns V42DET_NONE until the pattern is recognised,
 * then the result (also latched in d->result). d->last_char reports any
 * character completed on this bit so callers can buffer line data. */
int  v42det_rx_bit(v42det_rx_t *d, int bit);

#endif
