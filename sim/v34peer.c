/* Test peer: V.34 calling modem. See v34peer.h. */
#include <string.h>
#include "v34peer.h"
#include "../modem/p2ans.h"
#include "../modem/v8msg.h"

void v34peer_default(v34peer_cfg *cfg, int level)
{
    int k;
    memset(cfg, 0, sizeof *cfg);
    cfg->level = level;
    cfg->p2.level = level;
    cfg->p2.info0c.lo3000 = cfg->p2.info0c.hi3000 = cfg->p2.info0c.lo3200 = cfg->p2.info0c.hi3200 = 1;
    cfg->p2.info0c.pwr_reduce = 1; cfg->p2.info0c.c1664 = 1;
    cfg->p2.hi_carrier_mask = 1 << 4;
    for (k = 0; k < 6; k++) { cfg->p2.preemph[k] = 0; cfg->p2.max_rate[k] = -1; }
}

void v34peer_init(v34peer_t *p, const v34peer_cfg *cfg)
{
    v8_menu_t cm;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = cfg->modes ? cfg->modes : V8_MOD_V34;
    cm.n_mod_octets = 3;
    v8call_init(&p->v8, 8000, (int16_t)cfg->level, &cm);
    if (cfg->skip_v8) { p2c_init(&p->p2, &cfg->p2); p->state = VP_PHASE2; }
    else p->state = VP_V8;
}

static void start_dx(v34peer_t *p)
{
    v34dx_cfg c = p->cfg.dx;
    const info1a_t *i = &p->p2.info1a;
    int idx = i->down_sr;
    c.answer = 0;
    c.sym_rate = v34_symbol_rate(idx);
    v34_carrier(idx, i->hi_carrier, &c.tx_car_num, &c.tx_car_den);
    v34_carrier(idx, (p->cfg.p2.hi_carrier_mask >> idx) & 1, &c.rx_car_num, &c.rx_car_den);
    c.tx_preemph = i->preemph > 10 ? 0 : i->preemph;
    c.tx_pwr_red = i->min_pwr_red;
    c.level = p->cfg.level;
    c.rtd = p->p2.rtd;
    c.md_len = i->md_len;
    c.peer_1664 = p->p2.info0a.c1664;
    if (v34dx_init(&p->dx, &c) != 0) p->state = VP_ENDED;
    else p->state = VP_TRAINING;
}

int16_t v34peer_tick(v34peer_t *p, int16_t rx)
{
    int16_t out = 0;
    switch (p->state) {
    case VP_V8:
        if (v8call_tick(&p->v8, rx, &out) == V8C_DONE) {
            /* 11.2.1.1.1: INFO0c after the 75 ms of silence that follow CJ */
            p2c_init(&p->p2, &p->cfg.p2);
            p->state = VP_PHASE2;
        }
        break;
    case VP_PHASE2:
        out = p2c_tick(&p->p2, rx);
        if (p->p2.state == P2C_DONE) start_dx(p);
        else if (p->p2.state == P2C_FAILED) {
            /* 11.2.2.1: as a retrain, a few times */
            if (++p->p2_failed > 3) p->state = VP_ENDED;
            else { p->state = VP_RETRAIN; p->t = 0; }
        }
        break;
    case VP_TRAINING:
    case VP_DATA:
        out = v34dx_tick(&p->dx, rx);
        if (p->dx.state == DX_DATA) {
            if (p->state == VP_TRAINING) p->connects++;
            p->state = VP_DATA;
        } else if (p->dx.state == DX_RETRAIN) {
            p->retrains++;
            p->state = VP_RETRAIN; p->t = 0;
        } else if (p->dx.state == DX_FAILED) {
            p->state = VP_ENDED;
        } else if (p->dx.state < DX_P4_TRN) {
            p->state = VP_TRAINING;
        }
        break;
    case VP_RETRAIN:
        if (++p->t >= 560) {            /* 11.5.1: 70 ms of silence, then Tone B */
            p2c_init_retrain(&p->p2, &p->cfg.p2);
            p->state = VP_PHASE2;
        }
        break;
    default:
        break;
    }
    return out;
}
