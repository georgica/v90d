#include <math.h>
#include "v8call.h"

#define TWO_PI 6.283185307179586476925286766559

void v8call_init(v8call_t *c, int fs, int16_t amp, const v8_menu_t *cm)
{
    uint8_t oct[V8_MAX_OCTETS];
    int n = v8_encode(cm, oct, V8_MAX_OCTETS);
    c->state = V8C_WAIT; c->fs = fs; c->t = 0;
    v21mod_init(&c->mod, V21L_MARK, V21L_SPACE, fs, 300, amp);
    v21demod_init(&c->dem, V21H_MARK, V21H_SPACE, fs, 300);
    v8rx_init(&c->rx, fs);
    c->gi = c->gq = c->gph = 0; c->gn = 0; c->ghits = 0;
    c->ncm = v8_frame(V8_SYNC_CM_JM, oct, n, c->cmbits, 200);
    c->pos = 0; c->want_cj = 0; c->got_jm = 0; c->cm_sent = 0;
}

v8c_state v8call_tick(v8call_t *c, int16_t rx, int16_t *tx)
{
    int done, ev;
    *tx = 0;
    switch (c->state) {
    case V8C_WAIT:                               /* 2100 Hz for 10 x 20 ms blocks */
        c->gi += rx * cos(c->gph); c->gq += rx * sin(c->gph);
        c->gph += TWO_PI * 2100.0 / c->fs;
        if (++c->gn == 160) {
            double a = 2.0 * sqrt(c->gi * c->gi + c->gq * c->gq) / 160.0;
            c->ghits = (a > 300.0) ? c->ghits + 1 : 0;
            c->gi = c->gq = 0; c->gn = 0;
            if (c->ghits >= 10) { c->state = V8C_TE; c->t = 0; }
        }
        break;
    case V8C_TE:                                 /* Te = 1 s of silence */
        if (++c->t >= c->fs) { c->state = V8C_CM; c->t = 0; }
        break;
    case V8C_CM:
        ev = v8rx_tick(&c->rx, v21demod_tick(&c->dem, rx));
        if ((ev & V8RX_MENU) && !c->want_cj && v8_decode(c->rx.msg, c->rx.nmsg, &c->jm) == 0) {
            c->got_jm = 1; c->want_cj = 1;
        }
        *tx = v21mod_tick(&c->mod, c->cmbits[c->pos], &done);
        if (done) {
            if (++c->pos >= c->ncm) { c->pos = 0; c->cm_sent++; }
            /* CJ may start once the current octet (with start/stop) is complete */
            if (c->want_cj && (c->pos <= 10 || c->pos % 10 == 0)) { c->state = V8C_CJ; c->pos = 0; }
        }
        break;
    case V8C_CJ:                                 /* three framed zero octets */
        *tx = v21mod_tick(&c->mod, (c->pos % 10) == 9, &done);
        if (done && ++c->pos >= 30) { c->state = V8C_POST; c->t = 0; }
        break;
    case V8C_POST:
        if (++c->t >= c->fs * 75 / 1000) c->state = V8C_DONE;
        break;
    case V8C_DONE:
        break;
    }
    return c->state;
}
