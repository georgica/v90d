/* Test peer: a whole V.92 analogue modem call with its line. See v92call.h. */
#include <string.h>
#include <math.h>
#include "v92call.h"
#include "g711.h"
#include "../modem/pcmenc.h"
#include "../v90/v90_pcm.h"

#define DLY 20          /* samples from the digital modem's octet to the V.92 model */

static void line(v92call_t *c)
{
    static const double one[1] = { 1.0 };
    channel_cfg cc; v92chan_cfg vc;
    memset(&cc, 0, sizeof cc);
    cc.ds_noise_dbfs = -60.0; cc.us_noise_dbfs = c->cfg.us_noise_dbfs < 0 ? c->cfg.us_noise_dbfs : -60.0;
    cc.ds_taps = one; cc.ds_ntaps = 1; cc.us_taps = one; cc.us_ntaps = 1;
    cc.echo_gain = c->cfg.echo_gain; cc.echo_delay = c->cfg.echo_delay; cc.full_scale = 32124.0; cc.seed = 5;
    channel_init(&c->ch, &cc);
    memset(&vc, 0, sizeof vc);
    vc.alaw = c->cfg.alaw; vc.shape = c->cfg.shape;
    vc.level = c->cfg.up_level > 0 ? c->cfg.up_level : 4000;
    vc.noise = c->cfg.up_noise > 0 ? c->cfg.up_noise : 3;
    /* half of it, as the stage 2 calls ran (0.05 there against 0.1 on sim/channel) */
    vc.echo_gain = c->cfg.echo_gain_pcm > 0 ? c->cfg.echo_gain_pcm : c->cfg.echo_gain / 2; vc.echo_delay = c->cfg.echo_delay;
    vc.rx_linear = c->cfg.rx_linear;
    vc.up_delay = 24; vc.seed = 5;
    v92chan_init(&c->vch, &vc);
}

static void p2cfg(const v92call_t *c, p2a_cfg *pc, int retrain)
{
    memset(pc, 0, sizeof *pc);
    pc->level = c->cfg.level; pc->v90 = 1; pc->up_sr = -1; pc->uinfo = 91;
    pc->info0a.lo3000 = pc->info0a.hi3000 = 1; pc->info0a.lo3200 = pc->info0a.hi3200 = 1;
    pc->info0a.pwr_reduce = 1; pc->info0a.max_sr_diff = 5; pc->info0a.c1664 = 1;
    pc->info0a.v92 = (uint8_t)(c->cfg.v92 != 0);
    /* a retrain has no INFO0: what the first Phase 2 found holds (9.3/V.92) */
    pc->pcm_up = c->cfg.v92 && c->cfg.pcm_up && (!retrain || c->d_v92);
    pc->sections = 2; pc->ltot_code = 1; pc->lmax_code = 1; pc->md_len = 0;
}

void v92call_init(v92call_t *c, const v92call_cfg *cfg)
{
    v8_menu_t cm;
    int k;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (c->cfg.level <= 0) c->cfg.level = 4000;
    line(c);
    for (k = 0; k < (int)sizeof c->dly; k++) c->dly[k] = v90_pcm_octet(cfg->alaw, 0, 1);
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    if (cfg->lapm) { cm.has_prot = 1; cm.prot = V8_PROT_LAPM; cm.has_access = 1; cm.access = 0; }
    v8call_init(&c->v8, 8000, (int16_t)c->cfg.level, &cm);
    c->stage = VCL_V8;
}

/* Phase 2 is over: Phase 3 of what INFO1a selected */
static void phase3(v92call_t *c)
{
    c->phase2s++;
    if (c->phase2s == 1) c->d_v92 = c->p2.info0d.v92;
    dpskrx_init(&c->toneb, DPSK_FC_B);
    if (info1a_kind(&c->p2.info1a) == INFO1A_PCM) {
        v92a_cfg ac;
        memset(&ac, 0, sizeof ac);
        ac.alaw = c->cfg.alaw; ac.uinfo = c->p2.info1a.uinfo;
        ac.sil0 = 560; ac.ntrn1 = 2040; ac.ntrn1b = 2040; ac.jd_wait = 100;
        ac.dil_n = c->cfg.dil_n; ac.dil_syms = 1500; ac.trn2u_min = 12000;
        ac.up_rates = c->cfg.up_rates ? c->cfg.up_rates : 0x7FFFF;
        ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41;
        ac.getbit = c->cfg.getbit; ac.putbit = c->cfg.putbit; ac.user = c->cfg.user;
        v92a_init(&c->va, &ac);
        c->va.phase = 0.3;
        c->pcm_starts++;
        c->stage = VCL_PCM;
    } else {
        p34a_cfg qc;
        int sr = c->p2.info1a.up_sr, hi = c->p2.info1d.sr[sr].hi_carrier;
        memset(&qc, 0, sizeof qc);
        qc.alaw = c->cfg.alaw;
        qc.uinfo = c->p2.info1a.uinfo; qc.sym_rate = sr == 3 ? 3000 : 3200;
        qc.fc = sr == 3 ? (hi ? 2000.0 : 1800.0) : (hi ? 1920.0 : 3200.0 * 4 / 7);
        qc.gain = c->cfg.level; qc.sil0 = 70 * qc.sym_rate / 1000; qc.ntrn = 600; qc.jd_wait = 50;
        qc.dil_n = 8; qc.dil_syms = 1200;
        qc.Kt = 12; qc.Mt = 4; qc.Kd = 32; qc.Md = 41; qc.up_rates = 0x1FFF;
        qc.getbit = c->cfg.getbit; qc.putbit = c->cfg.putbit; qc.user = c->cfg.user;
        p34a_init(&c->pa, &qc);
        c->v90_starts++;
        c->stage = VCL_V90;
    }
}

int v92call_data(const v92call_t *c)
{
    if (c->hung) return 0;
    if (c->stage == VCL_PCM) return c->va.tx_ready && c->va.rx_ready && !c->va.in_reneg;
    if (c->stage == VCL_V90) return c->pa.tx_ready && c->pa.rx_ready;
    return 0;
}

void v92call_retrain(v92call_t *c)
{
    if (c->stage != VCL_PCM && c->stage != VCL_V90) return;
    c->retrains++; c->retraining = 1; c->rt_n = 0;
    c->ctx = 0;
    c->stage = VCL_RTSIL;
}

int v92call_renegotiate(v92call_t *c, int cleardown)
{
    if (c->stage == VCL_PCM) return v92a_renegotiate(&c->va, 0, cleardown);
    if (c->stage == VCL_V90) return p34a_renegotiate(&c->pa, 0, cleardown);
    return -1;
}

void v92call_hangup(v92call_t *c) { c->hung = 1; c->ctx = 0; }

static uint8_t tick(v92call_t *c, uint8_t ds_octet);

/* the octet a G.711 A/D gives, and next to it (rx_lin) what the network
 * hands over: that octet's value, or with a linear A/D the sample itself */
uint8_t v92call_tick(v92call_t *c, uint8_t ds_octet)
{
    uint8_t o = tick(c, ds_octet);
    if (!c->cfg.rx_linear) c->rx_lin = (int16_t)v90_octet_linear(c->cfg.alaw, o);
    return o;
}

int16_t v92call_tick_lin(v92call_t *c, uint8_t ds_octet)
{
    (void)v92call_tick(c, ds_octet);
    return c->rx_lin;
}

static uint8_t tick(v92call_t *c, uint8_t ds_octet)
{
    int alaw = c->cfg.alaw;
    double crx; uint8_t srv; int16_t x, o = 0;
    uint8_t late;

    c->n++;
    c->dly[c->dpos] = ds_octet;
    late = c->dly[(c->dpos + (int)sizeof c->dly - DLY) % (int)sizeof c->dly];
    c->dpos = (c->dpos + 1) % (int)sizeof c->dly;

    if (c->hung) {
        /* the same line, nobody on it */
        if (c->stage == VCL_PCM) return pcm_encode(alaw, c->rx_lin = v92chan_tick(&c->vch, ds_octet, 0, 0, 0));
        channel_tick(&c->ch, alaw ? linear_to_ulaw((int16_t)v90_octet_linear(1, ds_octet)) : ds_octet, 0, &crx, &srv);
        c->rx_lin = c->ch.us_lin;
        return alaw ? pcm_encode(1, (int16_t)ulaw_to_linear(srv)) : srv;
    }
    if (c->stage == VCL_PCM) {
        double amp = 0, frac = 0;
        int valid = v92a_tick(&c->va, late, &amp, &frac);
        int16_t rx;
        if (c->pcm_starts <= c->cfg.mute_pcm) valid = 0;
        rx = v92chan_tick(&c->vch, ds_octet, valid, amp, frac);
        c->rx_lin = rx;
        if (c->va.tx_ready && c->va.rx_ready) c->retraining = 0;
        /* 9.7.2.2: Tone B for more than 50 ms asks for a retrain */
        (void)dpskrx_tick(&c->toneb, (int16_t)v90_octet_linear(alaw, ds_octet));
        if (c->toneb.steady >= 400) v92call_retrain(c);
        return pcm_encode(alaw, rx);
    }

    channel_tick(&c->ch, alaw ? linear_to_ulaw((int16_t)v90_octet_linear(1, ds_octet)) : ds_octet, c->ctx, &crx, &srv);
    c->rx_lin = c->ch.us_lin;
    x = (int16_t)lround(crx);
    switch (c->stage) {
    case VCL_V8:
        if (v8call_tick(&c->v8, x, &o) == V8C_DONE) {
            if (c->v8.got_jm && v8_selected(&c->v8.jm) == V8_SEL_V90) {
                p2a_cfg pc;
                p2cfg(c, &pc, 0);
                p2a_init(&c->p2, &pc);
                c->stage = VCL_P2;
            } else {
                c->stage = c->v8.got_jm ? VCL_NO_PCM : VCL_FAILED;
            }
        }
        c->ctx = o;
        break;
    case VCL_P2:
        c->ctx = p2a_tick(&c->p2, x);
        if (c->p2.state == P2A_DONE) phase3(c);
        else if (c->p2.state == P2A_FAILED) { c->stage = VCL_FAILED; c->ctx = 0; }
        break;
    case VCL_V90:
        c->ctx = p34a_tick(&c->pa, ds_octet);
        if (c->pa.tx_ready && c->pa.rx_ready) c->retraining = 0;
        (void)dpskrx_tick(&c->toneb, x);
        if (c->toneb.steady >= 400) v92call_retrain(c);
        break;
    case VCL_RTSIL:
        /* 70 ms of silence, then Tone A and Phase 2 from 9.2.2.1.3/V.90 */
        c->ctx = 0;
        if (++c->rt_n >= 560) {
            p2a_cfg pc;
            p2cfg(c, &pc, 1);
            p2a_init_retrain(&c->p2, &pc);
            c->stage = VCL_P2;
        }
        break;
    default:
        c->ctx = 0;
        break;
    }
    return alaw ? pcm_encode(1, (int16_t)ulaw_to_linear(srv)) : srv;
}
