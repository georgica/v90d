#include <string.h>
#include "caller.h"

#define TX_BUSY_MARK 4096      /* pppc transmit backlog above which we report busy */

static int line_getbit(void *user)
{
    caller_t *c = user;
    return c->v42_on ? v42ec_tx_bit(&c->v42) : 1;
}

static void line_putbit(void *user, int bit)
{
    caller_t *c = user;
    if (c->v42_on) v42ec_rx_bit(&c->v42, bit);
}

void caller_cfg_default(caller_cfg_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->use_v8 = 1;
    cfg->modes = V8_MOD_V22 | V8_MOD_V21;
    cfg->allow2400 = 1;
    cfg->level = 3000;
    cfg->use_v42 = 1;
    v42ec_cfg_default(&cfg->v42, 1);
    cfg->ppp.user = "alice";
    cfg->ppp.pass = "wonderland";
    cfg->ppp.accept_auth = PPPC_AUTH_PAP | PPPC_AUTH_CHAP;
    cfg->ppp.send_accm = 1;
    cfg->ppp.accm = 0;
    cfg->ppp.pfc = cfg->ppp.acfc = 1;
    cfg->ppp.magic = 0x0badcafe;
    cfg->ppp.want_dns = 1;
    cfg->ppp.ccp = PPPC_CCP_PPPD;      /* like pppd: Deflate both ways */
}

/* levels of the V.90 calling modem: the ones sim/p2a and sim/p34a are
 * exercised with in the modem's own tests */
#define V90_LEVEL 4000

static void v90_begin(caller_t *c)
{
    v8_menu_t cm;
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA;
    cm.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21; cm.n_mod_octets = 3;
    cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
    v8call_init(&c->v8, 8000, V90_LEVEL, &cm);
    v14_init(&c->modem.dte);
    c->stage = CALLER_ST_V8;
}

static void v90_p2cfg(p2a_cfg *pc)
{
    memset(pc, 0, sizeof *pc);
    pc->level = V90_LEVEL; pc->v90 = 1; pc->up_sr = -1; pc->uinfo = 91;
    pc->info0a.lo3000 = pc->info0a.hi3000 = 1; pc->info0a.lo3200 = pc->info0a.hi3200 = 1;
    pc->info0a.pwr_reduce = 1; pc->info0a.max_sr_diff = 5; pc->info0a.c1664 = 1;
}

static void v90_phase2(caller_t *c)
{
    p2a_cfg pc;
    v90_p2cfg(&pc);
    p2a_init(&c->p2, &pc);
    c->stage = CALLER_ST_P2;
}

static void v90_phase3(caller_t *c)
{
    p34a_cfg qc;
    int sr = c->p2.info1a.up_sr, hi = c->p2.info1d.sr[sr].hi_carrier;
    memset(&qc, 0, sizeof qc);
    qc.alaw = c->cfg.alaw;
    qc.uinfo = 91; qc.sym_rate = sr == 3 ? 3000 : 3200;
    qc.fc = sr == 3 ? (hi ? 2000.0 : 1800.0) : (hi ? 1920.0 : 3200.0 * 4 / 7);
    qc.gain = V90_LEVEL; qc.sil0 = 70 * qc.sym_rate / 1000; qc.ntrn = 600; qc.jd_wait = 50;
    qc.dil_n = 8; qc.dil_syms = 1200;
    qc.Kt = 12; qc.Mt = 4; qc.Kd = 32; qc.Md = 41; qc.up_rates = 0x1FFF;
    if (c->cfg.use_v42) { qc.getbit = line_getbit; qc.putbit = line_putbit; qc.user = c; }
    else { qc.getbit = v14_getbit; qc.putbit = v14_putbit; qc.user = &c->modem.dte; }
    p34a_init(&c->p34, &qc);
    dpskrx_init(&c->toneb, DPSK_FC_B);
    c->stage = CALLER_ST_P34;
}

void caller_retrain(caller_t *c)
{
    if (c->stage == CALLER_ST_V92) { v92call_retrain(&c->vc); c->retrains = c->vc.retrains; c->retraining = c->vc.retraining; return; }
    if (c->stage != CALLER_ST_P34) return;
    c->retrains++; c->retraining = 1; c->rt_n = 0;
    c->stage = CALLER_ST_RTSIL;
}

/* the answerer's JM has no PCM: carry on as a V.22bis caller */
static void v90_fallback(caller_t *c)
{
    callpeer_init(&c->modem, 0, 0, c->cfg.allow2400, c->cfg.level);
    if (c->cfg.use_v42) callpeer_set_pump(&c->modem, line_getbit, line_putbit, c);
    c->stage = CALLER_ST_PEER;
}

void caller_init(caller_t *c, const caller_cfg_t *cfg)
{
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    c->cfg.v42.originator = 1;
    if (cfg->v92) {
        v92call_cfg vc;
        memset(&vc, 0, sizeof vc);
        vc.alaw = cfg->alaw; vc.v92 = 1; vc.pcm_up = cfg->v92 == 1; vc.level = V90_LEVEL;
        vc.lapm = cfg->use_v42; vc.rx_linear = cfg->v92_rx_linear;
        vc.echo_gain = 0.2; vc.echo_delay = 21; vc.dil_n = 40; vc.mute_pcm = cfg->v92_mute;
        if (cfg->use_v42) { vc.getbit = line_getbit; vc.putbit = line_putbit; vc.user = c; }
        else { vc.getbit = v14_getbit; vc.putbit = v14_putbit; vc.user = &c->modem.dte; }
        v14_init(&c->modem.dte);
        v92call_init(&c->vc, &vc);
        c->stage = CALLER_ST_V92;
    } else if (cfg->v32) {
        vc_cfg vc;
        memset(&vc, 0, sizeof vc);
        vc.level = cfg->level; vc.rates = V32_RALL; vc.trn_len = 1280; vc.user = c;
        if (cfg->use_v42) { vc.getbit = line_getbit; vc.putbit = line_putbit; }
        else { vc.getbit = v14_getbit; vc.putbit = v14_putbit; vc.user = &c->modem.dte; }
        v32call_init(&c->v32, &vc);
        c->v32_wait = 8000;
        c->stage = CALLER_ST_V32;
    } else if (cfg->v34) {
        v34peer_cfg pc;
        v34peer_default(&pc, cfg->level);
        if (cfg->use_v42) { pc.dx.getbit = line_getbit; pc.dx.putbit = line_putbit; pc.dx.user = c; }
        else { pc.dx.getbit = v14_getbit; pc.dx.putbit = v14_putbit; pc.dx.user = &c->modem.dte; }
        v34peer_init(&c->v34, &pc);
        c->stage = CALLER_ST_V34;
    } else if (cfg->v90) {
        v90_begin(c);
    } else {
        callpeer_init(&c->modem, cfg->use_v8, cfg->modes, cfg->allow2400, cfg->level);
        if (cfg->use_v42) callpeer_set_pump(&c->modem, line_getbit, line_putbit, c);
    }
    pppc_init(&c->ppp, &c->cfg.ppp);
}

int caller_link_up(const caller_t *c)
{
    int s;
    if (!c->cfg.use_v42) return c->connected;
    if (!c->v42_on) return 0;
    s = v42ec_state(&c->v42);
    return s == V42EC_CONNECTED || s == V42EC_ASYNC;
}

static void ppp_in(caller_t *c, const uint8_t *b, size_t n)
{
    size_t i;
    if (c->cfg.raw_rx) { c->cfg.raw_rx(c->cfg.ctx, b, n); return; }
    /* octet by octet so that no packet is missed: pppc only keeps the last */
    for (i = 0; i < n; i++) {
        pppc_input(&c->ppp, b + i, 1);
        if (c->ppp.ip_count != c->ip_seen) {
            c->ip_seen = c->ppp.ip_count;
            if (c->cfg.ip_rx) c->cfg.ip_rx(c->cfg.ctx, c->ppp.ip_last, c->ppp.ip_len);
        }
    }
}

static void pump(caller_t *c)
{
    uint8_t buf[256];
    size_t n;

    if (c->stash_n == 0)
        c->stash_n = !c->cfg.raw_tx ? pppc_output(&c->ppp, c->stash, sizeof c->stash) :
                     c->ppp_on ? c->cfg.raw_tx(c->cfg.ctx, c->stash, sizeof c->stash) : 0;
    if (c->cfg.use_v42) {
        while ((n = v42ec_read(&c->v42, buf, sizeof buf)) > 0) ppp_in(c, buf, n);
        n = v42ec_write(&c->v42, c->stash, c->stash_n);
    } else {
        int r;
        while ((r = v14_read(&c->modem.dte, buf, (int)sizeof buf)) > 0) ppp_in(c, buf, (size_t)r);
        n = (size_t)v14_write(&c->modem.dte, c->stash, (int)c->stash_n);
    }
    memmove(c->stash, c->stash + n, c->stash_n - n);
    c->stash_n -= n;
}

/* one sample of whichever calling modem is running */
static double modem_tick(caller_t *c, int16_t rx, uint8_t ds_octet)
{
    double tx = 0;
    int16_t o = 0;

    switch (c->stage) {
    case CALLER_ST_PEER:
        tx = callpeer_tick(&c->modem, rx);
        if (c->modem.connected && !c->connected) {
            c->connected = 1;
            c->tx_rate = c->rx_rate = c->modem.bitrate;
        }
        break;
    case CALLER_ST_V8:
        if (v8call_tick(&c->v8, rx, &o) == V8C_DONE) {
            if (!c->v8.got_jm) c->stage = CALLER_ST_FAILED;
            else if (v8_selected(&c->v8.jm) == V8_SEL_V90) v90_phase2(c);
            else if (v8_selected(&c->v8.jm) == 4) v90_fallback(c);
            else c->stage = CALLER_ST_FAILED;
        }
        tx = o;
        break;
    case CALLER_ST_P2:
        tx = p2a_tick(&c->p2, rx);
        if (c->p2.state == P2A_DONE) v90_phase3(c);
        else if (c->p2.state == P2A_FAILED) c->stage = CALLER_ST_FAILED;
        break;
    case CALLER_ST_P34:
        tx = p34a_tick(&c->p34, ds_octet);
        if (c->p34.tx_ready && c->p34.rx_ready) {
            c->retraining = 0;
            if (!c->connected) {
                c->connected = 1;
                c->tx_rate = c->p34.up_rate;
                c->rx_rate = (c->p34.cp.drn + 20) * 8000 / 6;
            }
        }
        /* 9.5.2.2: Tone B for more than 50 ms asks for a retrain */
        (void)dpskrx_tick(&c->toneb, rx);
        if (c->toneb.steady >= 400) caller_retrain(c);
        break;
    case CALLER_ST_V32:
        if (c->v32_wait > 0) { c->v32_wait--; break; }
        tx = v32call_tick(&c->v32, rx);
        if (c->v32.connected) {
            c->connected = 1;
            c->tx_rate = c->rx_rate = c->v32.bitrate;
            c->retraining = c->v32.state != VCS_DATA;
        }
        if (c->v32.state == VCS_FAILED) c->stage = CALLER_ST_FAILED;
        break;
    case CALLER_ST_V34:
        tx = v34peer_tick(&c->v34, rx);
        if (c->v34.state == VP_DATA) {
            c->retraining = 0;
            c->connected = 1;
            c->tx_rate = c->v34.dx.tx_rate; c->rx_rate = c->v34.dx.rx_rate;
        } else if (c->connected) {
            c->retraining = 1;
        }
        if (c->v34.state == VP_ENDED) c->stage = CALLER_ST_FAILED;
        break;
    case CALLER_ST_RTSIL:
        /* 70 ms of silence, then Tone A and Phase 2 from 9.2.2.1.3 */
        if (++c->rt_n >= 560) {
            p2a_cfg pc;
            v90_p2cfg(&pc);
            p2a_init_retrain(&c->p2, &pc);
            c->stage = CALLER_ST_P2;
        }
        break;
    default:
        break;
    }
    return tx;
}

/* what follows a modem sample: error control, PPP, the millisecond clock */
static void after_modem(caller_t *c)
{
    if (c->cfg.use_v42 && !c->v42_on && c->connected) {
        /* as dce.c does: T401 long enough for the line rates */
        long fb = (c->cfg.v42.lapm.n401_tx + 8) * 8L;
        long t401 = fb * 1000 / c->tx_rate + 2 * fb * 1000 / c->rx_rate + 300;
        if (!c->cfg.fixed_timers && c->cfg.v42.lapm.t401_ms < t401) c->cfg.v42.lapm.t401_ms = (int)t401;
        /* V.92 9.3.1: both modems V.92 (INFO0) and LAPM in CM and JM: no ODP / ADP */
        if (c->stage == CALLER_ST_V92 && c->vc.d_v92 && c->vc.v8.jm.has_prot && c->vc.v8.jm.prot == V8_PROT_LAPM) {
            c->cfg.v42.detect = 0;
            c->odp_bypass = 1;
        }
        v42ec_init(&c->v42, &c->cfg.v42);
        c->v42_on = 1;
    }
    if (++c->sub < 8) return;
    c->sub = 0;
    c->ms++;
    if (c->retraining) return;          /* a modem holds its error control while it retrains */
    if (c->v42_on) v42ec_tick(&c->v42, 1);
    if (!c->ppp_on && caller_link_up(c)) {
        if (!c->cfg.fixed_timers) {
            /* as dce.c does: a Restart timer that covers the round trip */
            uint32_t rt = 1000000u / (uint32_t)c->tx_rate + 1000000u / (uint32_t)c->rx_rate + 1500u;
            if (c->ppp.lcp.restart_ms < rt) c->ppp.lcp.restart_ms = c->ppp.ipcp.restart_ms = rt;
            if (c->ppp.ccp.restart_ms < rt) c->ppp.ccp.restart_ms = rt;
            c->ppp.cfg.ccp_reset_ms = c->ppp.ccp.restart_ms + 15000000u / (uint32_t)c->tx_rate + 15000000u / (uint32_t)c->rx_rate;
        }
        if (!c->cfg.raw_tx) pppc_start(&c->ppp);
        c->ppp_on = 1;
    }
    if (c->ppp_on) {
        if (!c->cfg.raw_tx) pppc_tick(&c->ppp, 1);
        pump(c);
    }
}

double caller_tick_pcm(caller_t *c, int16_t rx, uint8_t ds_octet)
{
    double tx;

    if (c->on_hook) return 0;
    tx = modem_tick(c, rx, ds_octet);
    after_modem(c);
    return tx;
}

uint8_t caller_tick_v92(caller_t *c, uint8_t ds_octet)
{
    uint8_t o = v92call_tick(&c->vc, ds_octet);
    if (c->on_hook) return o;
    c->retrains = c->vc.retrains;
    if (c->vc.retraining) c->retraining = 1;
    if (v92call_data(&c->vc)) {
        c->retraining = 0;
        c->connected = 1;
        if (c->vc.stage == VCL_PCM) { c->tx_rate = c->vc.va.up_rate; c->rx_rate = c->vc.va.down_rate; }
        else { c->tx_rate = c->vc.pa.up_rate; c->rx_rate = (c->vc.pa.cp.drn + 20) * 8000 / 6; }
    }
    if (c->vc.stage == VCL_FAILED || c->vc.stage == VCL_NO_PCM) c->stage = CALLER_ST_FAILED;
    after_modem(c);
    return o;
}

int16_t caller_tick(caller_t *c, int16_t rx)
{
    double tx = caller_tick_pcm(c, rx, 0xFF);
    return (int16_t)(tx > 32767 ? 32767 : tx < -32768 ? -32768 : tx);
}

int caller_send_ip(caller_t *c, const uint8_t *pkt, size_t len)
{
    if (c->ppp.tx_n > (size_t)(c->cfg.tx_busy_mark ? c->cfg.tx_busy_mark : TX_BUSY_MARK)) return -2;
    return pppc_send_ip(&c->ppp, pkt, len) == 0 ? 0 : -1;
}

void caller_hangup(caller_t *c) { c->on_hook = 1; }
