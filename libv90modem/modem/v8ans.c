#include "v8ans.h"

#define MS(v, ms) ((int32_t)((int64_t)(ms) * (v)->fs / 1000))

/* carrier squelch: below roughly -46 dBm0 the V.21(L) receiver reads idle mark
 * (V.21 carrier detector: ON above -43 dBm, OFF below -48 dBm) */
#define SQUELCH_AMP 110

void v8ans_init(v8ans_t *v, int fs, int16_t amp, const v8_menu_t *local)
{
    v->state = V8A_SILENCE; v->fs = fs; v->t = 0;
    ansam_init(&v->ansam, fs, amp, 1);          /* V.90 9.1.3.1: with phase reversals */
    v21mod_init(&v->mod, V21H_MARK, V21H_SPACE, fs, 300, amp);
    v21demod_init(&v->dem, V21L_MARK, V21L_SPACE, fs, 300);
    v8rx_init(&v->rx, fs);
    v->squelch = v21demod_energy_of(&v->dem, SQUELCH_AMP);
    v->local = *local;
    v->njm = v->jmpos = 0;
    v->result = V8A_RES_NONE; v->selected = 0;
}

static void go(v8ans_t *v, v8a_state s) { v->state = s; v->t = 0; }

v8a_state v8ans_tick(v8ans_t *v, int16_t rx, int16_t *tx)
{
    int level, ev = 0, done;
    uint8_t oct[V8_MAX_OCTETS];

    *tx = 0;
    if (v->state == V8A_ANSAM || v->state == V8A_JM) {
        level = v21demod_tick(&v->dem, rx);
        if (v->dem.energy < v->squelch) level = 1;
        ev = v8rx_tick(&v->rx, level);
    }

    switch (v->state) {
    case V8A_SILENCE:
        if (++v->t >= MS(v, V8A_SILENCE_MS)) go(v, V8A_ANSAM);
        break;
    case V8A_ANSAM:
        ansam_run(&v->ansam, tx, 1);
        v->t++;
        if ((ev & V8RX_MENU) && v8_decode(v->rx.msg, v->rx.nmsg, &v->cm) == 0) {
            int n;
            v8_build_jm(&v->cm, &v->local, &v->jm);
            v->selected = v8_selected(&v->jm);
            n = v8_encode(&v->jm, oct, V8_MAX_OCTETS);
            v->njm = v8_frame(V8_SYNC_CM_JM, oct, n, v->jmbits, V8A_MAXBITS);
            v->jmpos = 0;
            go(v, V8A_JM);
        } else if (v->t >= MS(v, V8A_ANSAM_MS)) {
            v->result = V8A_RES_NO_CM;
            go(v, V8A_POST);
        }
        break;
    case V8A_JM:
        *tx = v21mod_tick(&v->mod, v->jmbits[v->jmpos], &done);
        if (done && ++v->jmpos >= v->njm) v->jmpos = 0;
        v->t++;
        if (ev & V8RX_CJ) {                      /* JM stops at once (8.2.3) */
            v->result = V8A_RES_OK;
            go(v, V8A_POST);
        } else if (v->t >= MS(v, V8A_JM_MS)) {
            v->result = V8A_RES_NO_CJ;
            go(v, V8A_POST);
        }
        break;
    case V8A_POST:
        if (++v->t >= MS(v, V8A_POST_MS)) go(v, V8A_DONE);
        break;
    case V8A_DONE:
        break;
    }
    return v->state;
}
