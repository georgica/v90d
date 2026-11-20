/* Real-modem test rig core: the answering modem behind a mu-law frame
 * interface, with a tiny "terminal" on its data side: on connect it sends a
 * banner, then echoes every received character. Used by rig/sipmodem.c (which
 * rides the stock firmware's SIP/RTP path; a TEST RIG, never part of the
 * shipped image) and by the host tests. */
#ifndef V90D_RIG_MODEMTERM_H
#define V90D_RIG_MODEMTERM_H
#include "v90modem.h"

typedef struct {
    v90m_t       *ans;     /* in ans_mem */
    v90m_async_t *dte;     /* in dte_mem */
    union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } ans_mem;
    union { v90m_align a; uint8_t b[V90M_ASYNC_MEM_SIZE]; } dte_mem;
    int      was_connected, reported_fail;
    unsigned long rx_chars, tx_chars, frames;
    /* log callback (may be NULL): one line of text, no newline */
    void   (*log)(const char *line);
    char     linebuf[96]; int linelen;     /* received text, logged per line */
} mterm_t;

void mterm_init(mterm_t *m, int level, unsigned modulations, void (*log)(const char *));   /* V90M_MOD_* */
/* process n mu-law samples from the line, produce n mu-law samples to the line */
void mterm_frame(mterm_t *m, const uint8_t *in, uint8_t *out, int n);

#endif
