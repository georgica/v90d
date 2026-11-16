/* Async (start/stop) character receiver working on a per-sample line-level
 * stream (1 = mark, 0 = space) from an FSK demodulator. Bit timing is taken
 * from each start-bit edge and bits are sliced mid-bit, so no separate baud
 * recovery is needed. 8 data bits, LSB first, 1 stop bit. Integer only. */
#ifndef V90D_MODEM_ASYNC_H
#define V90D_MODEM_ASYNC_H
#include <stdint.h>

#define ARX_NONE     (-1)
#define ARX_FRAMEERR (-2)

typedef struct {
    int32_t  fs, baud;
    int      state;        /* 0 idle, 1 in character */
    uint32_t acc;          /* bit-phase accumulator (units of baud; wraps at fs) */
    int      nbit;
    uint8_t  sh;
    int32_t  mark_run;     /* samples of continuous mark since the last character */
    int32_t  pre_run;      /* mark_run latched at the start edge of the current char */
} arx_t;

void arx_init(arx_t *a, int fs, int baud);

/* One sample of line level. Returns a character 0..255 when its stop bit is
 * sliced, ARX_FRAMEERR on a bad stop bit, else ARX_NONE. */
int arx_tick(arx_t *a, int level);

/* mark time (in whole bit periods) that preceded the character just returned */
int arx_pre_bits(const arx_t *a);
/* mark time (bit periods) since the last character, while idle */
int arx_idle_bits(const arx_t *a);

#endif
