/* See pppcall.h. */
#include <stdio.h>
#include <string.h>
#include "pppcall.h"

static uint16_t csum(const uint8_t *p, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

/* an IP packet from the caller: answer a ping to our address, count the rest */
static void ip_rx(void *ctx, const uint8_t *pkt, size_t len)
{
    pppcall_t *c = (pppcall_t *)ctx;
    static uint8_t r[1500];
    size_t ihl = len >= 20 ? (size_t)(pkt[0] & 15) * 4 : 0;
    uint32_t dst = len >= 20 ? (uint32_t)pkt[16] << 24 | (uint32_t)pkt[17] << 16 | (uint32_t)pkt[18] << 8 | pkt[19] : 0;
    uint16_t k;
    if (len > sizeof r || ihl < 20 || len < ihl + 8 || (pkt[0] >> 4) != 4 || pkt[9] != 1 || pkt[ihl] != 8 ||
        dst != PPPCALL_LOCAL || ((pkt[6] & 0x3F) | pkt[7]) != 0) {
        c->other_ip++;
        return;
    }
    memcpy(r, pkt, len);
    memcpy(r + 12, pkt + 16, 4); memcpy(r + 16, pkt + 12, 4);   /* swap the addresses */
    r[8] = 64; r[10] = r[11] = 0;
    k = csum(r, ihl); r[10] = (uint8_t)(k >> 8); r[11] = (uint8_t)k;
    r[ihl] = 0; r[ihl + 2] = r[ihl + 3] = 0;                    /* echo reply */
    k = csum(r + ihl, len - ihl); r[ihl + 2] = (uint8_t)(k >> 8); r[ihl + 3] = (uint8_t)k;
    if (dce_send_ip(&c->dce, r, len) == 0) c->pings++;
}

void pppcall_init(pppcall_t *c, const pppcall_cfg *cfg)
{
    dce_cfg_t d;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    dce_cfg_default(&d);
    d.tx_level = cfg->level; d.modulations = cfg->modulations; d.alaw = cfg->alaw;
    d.echo_delay = -1;                          /* the RTP path is long: let the modem measure */
    if (cfg->user) {
        c->users[0].name = cfg->user; c->users[0].secret = cfg->pass ? cfg->pass : "";
        d.ppp.auth = PPP_AUTH_CHAP | PPP_AUTH_PAP; d.ppp.users = c->users; d.ppp.n_users = 1;
    }
    d.ppp.our_ip = PPPCALL_LOCAL; d.ppp.peer_ip = PPPCALL_PEER;
    d.ppp.dns1 = PPPCALL_LOCAL;
    d.ppp.seed = 0x5eed1234u;
    d.ppp.ctx = c; d.ppp.ip_rx = ip_rx;
    dce_init(&c->dce, &d);
    dce_get_status(&c->dce, &c->shown);
    c->alive = 1;
}

static const char *const state_name[] = { "training", "negotiating", "online", "closing", "ended" };
static const char *const mod_name[] = { "-", "V.21", "V.22", "V.22bis", "V.90" };
static const char *const phase_name[] = { "dead", "establish", "authenticate", "network", "terminate" };

static void report(pppcall_t *c)
{
    char line[200];
    const dce_status_t *s = &c->st, *o = &c->shown;
    if (!c->cfg.log) return;
    if (s->modem_connected && !o->modem_connected) {
        snprintf(line, sizeof line, "CONNECT %s, %d bit/s to the caller, %d bit/s from it, %.1f s after the answer",
                 mod_name[s->modulation >= 0 && s->modulation <= 4 ? s->modulation : 0], s->bitrate, s->rx_bitrate, s->connect_ms / 1000.0);
        c->cfg.log(line);
    }
    if (s->state != o->state || s->ppp_phase != o->ppp_phase || s->error_control != o->error_control ||
        s->ip_up != o->ip_up || s->v42_state != o->v42_state) {
        snprintf(line, sizeof line, "%s: V.42 %s%s, PPP %s%s%s%s", state_name[s->state],
                 s->error_control ? "LAPM" : s->v42_state == V90M_EC_ASYNC ? "none (start-stop)" : "detecting",
                 !(s->comp_tx || s->comp_rx) ? "" : s->comp_alg == V90M_EC_COMP_V44 ? " + V.44" : " + V.42bis",
                 (unsigned)s->ppp_phase < 5 ? phase_name[s->ppp_phase] : "?",
                 s->ip_up ? ", IP up" : "", s->peer_name[0] ? ", user " : "", s->peer_name);
        c->cfg.log(line);
    }
    if (s->state == DCE_ENDED && o->state != DCE_ENDED) {
        snprintf(line, sizeof line, "call over after %.1f s: %s; %lu pings answered, %lu other IP packets, LAPM %lu frames resent, %lu bad",
                 s->call_ms / 1000.0, dce_end_name(s->end_reason), c->pings, c->other_ip, s->lapm_retx, s->lapm_rx_bad);
        c->cfg.log(line);
    }
    c->shown = *s;
}

void pppcall_frame(pppcall_t *c, const uint8_t *rx, uint8_t *tx, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (c->alive) c->alive = dce_tick_pcm(&c->dce, rx[i], &tx[i]);
        else tx[i] = c->cfg.alaw ? 0xD5 : 0xFF;
        if (++c->sub >= 800) {                  /* ten times a second */
            c->sub = 0;
            dce_get_status(&c->dce, &c->st);
            report(c);
        }
    }
}
