/* V.8 answer-side start-up (V.8 8.2, V.90 9.1.3):
 *   >= 0.2 s silence -> ANSam (with phase reversals) while listening for CM on
 *   V.21(L) -> on >= 2 identical CM: JM on V.21(H) until CJ -> 75 ms silence.
 * Sample-driven, fixed-point, no allocation. */
#ifndef V90D_MODEM_V8ANS_H
#define V90D_MODEM_V8ANS_H
#include "ansam.h"
#include "v21.h"
#include "v8rx.h"

typedef enum { V8A_SILENCE, V8A_ANSAM, V8A_JM, V8A_POST, V8A_DONE } v8a_state;

#define V8A_RES_NONE  0
#define V8A_RES_OK    1   /* CM/JM/CJ completed: see jm, selected */
#define V8A_RES_NO_CM 2   /* ANSam ran its 5 s unanswered: caller is a pre-V.8
                             modem; continue as a legacy answerer (V.32bis Annex A) */
#define V8A_RES_NO_CJ 3   /* JM sent but CJ never seen */

#define V8A_SILENCE_MS 200
#define V8A_ANSAM_MS   5000
#define V8A_JM_MS      5000    /* our bound on JM (V.8 8.2.3 leaves it open) */
#define V8A_POST_MS    75
#define V8A_MAXBITS    (20 + 10 * V8_MAX_OCTETS)

typedef struct {
    v8a_state  state;
    int32_t    fs, t;              /* t = samples spent in the current state */
    ansam_t    ansam;
    v21mod_t   mod;
    v21demod_t dem;
    v8rx_t     rx;
    int64_t    squelch;
    v8_menu_t  local, cm, jm;
    uint8_t    jmbits[V8A_MAXBITS];
    int        njm, jmpos;
    int        result, selected;
} v8ans_t;

void v8ans_init(v8ans_t *v, int fs, int16_t amp, const v8_menu_t *local);
/* one sample in, one sample out; returns the state after this sample */
v8a_state v8ans_tick(v8ans_t *v, int16_t rx, int16_t *tx);

#endif
