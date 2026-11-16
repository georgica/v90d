/* Test peer: a complete CALLING modem built from the library's own primitives:
 * optional V.8, then V.22bis/V.22 or V.21, with a byte interface. x86 test side. */
#ifndef V90D_SIM_CALLPEER_H
#define V90D_SIM_CALLPEER_H
#include "v8call.h"
#include "../modem/v22bis.h"
#include "../modem/v14.h"
#include "../modem/async.h"

typedef enum { CP_V8, CP_V22, CP_V21, CP_FAILED } cp_phase;

typedef struct {
    cp_phase  phase;
    int       use_v8, allow2400, level;
    v8call_t  v8;
    v22bis_t  v22;
    v21mod_t  fmod; v21demod_t fdem; arx_t uart; int fbit;
    v14_t     dte;
    int       connected, bitrate;
    /* line bit pump; callpeer_init points it at the built-in V.14 terminal
     * (dte), callpeer_set_pump replaces it */
    v22_getbit_fn getbit; v22_putbit_fn putbit; void *pump_user;
} callpeer_t;

/* modes: V8_MOD_* offered in CM (ignored without V.8, where it is a V.22bis caller) */
void    callpeer_init(callpeer_t *c, int use_v8, unsigned modes, int allow2400, int level);
int16_t callpeer_tick(callpeer_t *c, int16_t rx);
/* Optional, after callpeer_init: take the line bit stream over (for example
 * to run an error-control layer on the calling side). getbit is called for
 * every transmitted line bit, putbit for every received one; c->dte is then
 * unused. In V.21 mode received characters arrive as ten start-stop bits. */
void    callpeer_set_pump(callpeer_t *c, v22_getbit_fn getbit, v22_putbit_fn putbit, void *user);

#endif
