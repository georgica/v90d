#include "callpeer.h"

static void start_v22(callpeer_t *c)
{
    v22m_init(&c->v22, V22_CALLER, c->allow2400, c->level, c->getbit, c->putbit, c->pump_user);
    c->phase = CP_V22;
}

void callpeer_init(callpeer_t *c, int use_v8, unsigned modes, int allow2400, int level)
{
    v8_menu_t cm;
    c->use_v8 = use_v8; c->allow2400 = allow2400; c->level = level;
    c->connected = 0; c->bitrate = 0; c->fbit = 1;
    v14_init(&c->dte);
    c->getbit = v14_getbit; c->putbit = v14_putbit; c->pump_user = &c->dte;
    cm.call_function = V8_CALLF_DATA; cm.modes = (uint16_t)modes; cm.n_mod_octets = 3;
    cm.has_pcm = cm.pcm = 0; cm.has_prot = cm.prot = 0; cm.has_access = cm.access = 0;
    v8call_init(&c->v8, 8000, (int16_t)(level * 181 / 128), &cm);
    if (use_v8) c->phase = CP_V8; else start_v22(c);
}

int16_t callpeer_tick(callpeer_t *c, int16_t rx)
{
    int16_t tx = 0;
    int done, ch, i, sel;
    switch (c->phase) {
    case CP_V8:
        if (v8call_tick(&c->v8, rx, &tx) != V8C_DONE) break;
        sel = c->v8.got_jm ? v8_selected(&c->v8.jm) : 0;
        if (sel == 4) start_v22(c);
        else if (sel == 12) {
            v21mod_init(&c->fmod, V21L_MARK, V21L_SPACE, 8000, 300, (int16_t)(c->level * 181 / 128));
            v21demod_init(&c->fdem, V21H_MARK, V21H_SPACE, 8000, 300);
            arx_init(&c->uart, 8000, 300);
            c->phase = CP_V21; c->connected = 1; c->bitrate = 300;
        } else c->phase = CP_FAILED;
        break;
    case CP_V22:
        tx = v22m_tick(&c->v22, rx);
        if (c->v22.tx_ready && c->v22.rx_ready) { c->connected = 1; c->bitrate = c->v22.bitrate; }
        break;
    case CP_V21:
        ch = arx_tick(&c->uart, v21demod_tick(&c->fdem, rx));
        if (ch >= 0) {
            c->putbit(c->pump_user, 0);
            for (i = 0; i < 8; i++) c->putbit(c->pump_user, (ch >> i) & 1);
            c->putbit(c->pump_user, 1);
        }
        tx = v21mod_tick(&c->fmod, c->fbit, &done);
        if (done) c->fbit = c->getbit(c->pump_user) & 1;
        break;
    case CP_FAILED:
        break;
    }
    return tx;
}

void callpeer_set_pump(callpeer_t *c, v22_getbit_fn getbit, v22_putbit_fn putbit, void *user)
{
    c->getbit = getbit; c->putbit = putbit; c->pump_user = user;
    if (c->phase == CP_V22) {          /* started without V.8: already wired */
        c->v22.tx.getbit = getbit; c->v22.tx.user = user;
        c->v22.rx.putbit = putbit; c->v22.rx.user = user;
    }
}
