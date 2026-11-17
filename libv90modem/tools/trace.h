/* tools/trace.c: the modem's side of a replayed call (live/replay in v90d,
 * tools/v90m_replay.c here). Built with -DP34D_TRACE. Not installed. */
#ifndef V90M_TOOLS_TRACE_H
#define V90M_TOOLS_TRACE_H
#include "../modem/answer.h"

extern int tr_on;               /* print and trace: 1 in the pass that reports */
extern long tr_t;               /* samples seen by the V.90 / V.92 engines' hooks */
extern int16_t rec_tx;          /* the recorded transmit sample (linear) ... */
extern int rec_tx_on;           /* ... used as the V.34 canceller's reference while this is set */
extern int rec_tx92;            /* set by the V.92 engine reaching data mode: likewise for it */
extern const char *trace_bits_path;

void trace_reset(void);
void trace_a(answer_t *a, long src);    /* phase, V.8, Phase 2 */
void trace_b(answer_t *a, long src);    /* V.34, V.92, Phases 3 and 4 */
#endif
