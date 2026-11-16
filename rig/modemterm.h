/* Real-modem test rig core: the answering modem behind a mu-law frame
 * interface, with a tiny "terminal" on its data side: on connect it sends a
 * banner, then echoes every received character. Used by rig/sipmodem.c (which
 * rides the stock firmware's SIP/RTP path; a TEST RIG, never part of the
 * shipped image) and by the host tests. */
#ifndef V90D_RIG_MODEMTERM_H
#define V90D_RIG_MODEMTERM_H
#include "../modem/answer.h"
#include "../modem/v14.h"

typedef struct {
    answer_t ans;
    v14_t    dte;
    int      was_connected, reported_fail;
    unsigned long rx_chars, tx_chars, frames;
    /* log callback (may be NULL): one line of text, no newline */
    void   (*log)(const char *line);
    char     linebuf[96]; int linelen;     /* received text, logged per line */
} mterm_t;

void mterm_init(mterm_t *m, int level, unsigned modes, void (*log)(const char *));
/* process n mu-law samples from the line, produce n mu-law samples to the line */
void mterm_frame(mterm_t *m, const uint8_t *in, uint8_t *out, int n);

#endif
