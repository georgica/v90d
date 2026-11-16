#include "async.h"

#define RUN_CAP 1000000

void arx_init(arx_t *a, int fs, int baud)
{
    a->fs = fs; a->baud = baud;
    a->state = 0; a->acc = 0; a->nbit = 0; a->sh = 0;
    a->mark_run = 0; a->pre_run = 0;
}

int arx_tick(arx_t *a, int level)
{
    switch (a->state) {
    case 0:                                   /* idle: wait for a start edge */
        if (level) { if (a->mark_run < RUN_CAP) a->mark_run++; return ARX_NONE; }
        a->state = 1; a->nbit = 0; a->sh = 0;
        a->pre_run = a->mark_run;
        a->acc = (uint32_t)a->fs / 2;         /* first slice half a bit from the edge */
        return ARX_NONE;
    case 1:
        a->acc += (uint32_t)a->baud;
        if (a->acc < (uint32_t)a->fs) return ARX_NONE;
        a->acc -= (uint32_t)a->fs;
        if (a->nbit == 0) {                   /* middle of the start bit */
            if (level) { a->state = 0; a->mark_run = a->pre_run; return ARX_NONE; } /* glitch */
            a->nbit = 1;
            return ARX_NONE;
        }
        if (a->nbit <= 8) {
            a->sh = (uint8_t)((a->sh >> 1) | (level ? 0x80 : 0));
            a->nbit++;
            return ARX_NONE;
        }
        a->mark_run = 0;
        if (level) { a->state = 0; return a->sh; }
        a->state = 2;                         /* bad stop: wait for mark */
        return ARX_FRAMEERR;
    default:
        if (level) a->state = 0;
        return ARX_NONE;
    }
}

int arx_pre_bits(const arx_t *a)  { return (int)((int64_t)a->pre_run * a->baud / a->fs); }
int arx_idle_bits(const arx_t *a) { return (int)((int64_t)a->mark_run * a->baud / a->fs); }
