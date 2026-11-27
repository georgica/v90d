/* The dial-up server without its operating system: see srv.h. */
#include <stdio.h>
#include <string.h>
#include "srv.h"

/* RMS of a tone of 0 dBm0 in 16-bit sample units (mu-law: a sine of
 * +3.17 dBm0 just reaches the top codeword, 32124) */
#define RMS_0DBM0 15770

/* 10 log10(num / den) in 0.1 dB, integer only: for the line level shown on
 * the status page (the same arithmetic as the modem's own probe_db10) */
static int32_t log2q8(int64_t v)
{
    static const uint16_t tab[17] = { 0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193,
                                      207, 220, 232, 244, 256 };
    int n = 0, i, f;
    int64_t t = v;
    if (v <= 0) return -(64 << 8);
    while (t > 1) { t >>= 1; n++; }
    /* 8 mantissa bits below the leading one */
    t = n >= 8 ? (v >> (n - 8)) & 0xff : (v << (8 - n)) & 0xff;
    i = (int)(t >> 4); f = (int)(t & 15);
    return (n << 8) + tab[i] + (((tab[i + 1] - tab[i]) * f) >> 4);
}

static int db10(int64_t num, int64_t den)
{
    /* 10 log10(2) = 3.0103 dB per octave -> 30.103 tenths; / 256 for Q8 */
    return (int)(((int64_t)(log2q8(num) - log2q8(den)) * 30103) / 256000);
}

int srv_level_rms(int dbm0)
{
    /* 10^(-1/20) in Q15 per dB below 0 dBm0 */
    int32_t v = RMS_0DBM0;
    if (dbm0 > 0) dbm0 = 0;
    for (; dbm0 < 0; dbm0++) v = (v * 29205 + 16384) >> 15;
    return (int)v;
}

static uint8_t silence(const srv_t *s) { return v90m_g711_encode(s->cfg.law == CFG_LAW_ALAW, 0); }

/* the port is set up for a game console: config.h, console.h */
static int for_console(const cfg_t *c) { return c->service == CFG_SERVICE_DREAMCAST; }
/* two consoles joined modem to modem: saturn.h */
static int for_link(const cfg_t *c) { return c->service == CFG_SERVICE_SATURN; }
/* every [modem] and [ppp] option applies: an Internet or Archive port, or a
 * BBS call on any port. A console port or a console link runs a fixed set
 * (no V.92, x2, K56flex, PPP compression or encryption) whatever is set. */
static int full_options(const cfg_t *c, int bbs) { return bbs || (!for_console(c) && !for_link(c)); }

static void raw_from_caller(void *ctx, const uint8_t *oct, size_t n)
{
    srv_t *s = (srv_t *)ctx;
    tel_activity(&s->tel);
    s->status.rx_octets += n;
    if (s->raw_out) s->raw_out(s->ctx, oct, n);
}

void srv_set_raw(srv_t *s, srv_raw_fn raw_out) { s->raw_out = raw_out; }
const char *srv_dialed(const srv_t *s) { return s->tel.dialed; }
void srv_set_svc_lookup(srv_t *s, srv_svc_fn known) { s->svc_known = known; }

int srv_svc_number(const srv_t *s, const char *dialed)
{
    const cfg_t *c = &s->cfg;
    int i;
    if (!dialed[0]) return 0;
    for (i = 0; i < c->n_services && i < CFG_MAX_SERVICES; i++)
        if (cfg_svc_number_match(c->services[i].number, dialed)) return SRV_SVC_OWN;
    if (c->svc_netstalgia && s->svc_known && s->svc_known(dialed) == 2) return SRV_SVC_LIST;
    return 0;
}

void srv_set_protoweb(srv_t *s, int on) { s->protoweb_ok = on != 0; }
void srv_set_entropy(srv_t *s, const uint8_t *b, size_t n)
{
    if (n > sizeof s->entropy) n = sizeof s->entropy;
    memcpy(s->entropy, b, n); s->entropy_n = n;
}
int  srv_protoweb(const srv_t *s) { return s->in_call && s->protoweb; }
int  srv_webtv(const srv_t *s) { return s->in_call && s->webtv; }

size_t srv_mppe_note(const ppp_counters_t *k, char *buf, size_t n)
{
    uint32_t v;
    int r;
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!k || !(k->mppe_seen & PPP_MPPE_SEEN_REQ) || k->comp_tx == PPP_COMP_MPPE) return 0;
    v = k->mppe_peer_req;
    if (!(v & 0xe0u)) return 0;                         /* S, M, L: no key length asked */
    r = snprintf(buf, n, "the caller asked for encryption (option 18 %08lx:%s%s%s%s) and none was agreed",
                 (unsigned long)v,
                 v & 0x40u ? " 128-bit," : "", v & 0x80u ? " 56-bit," : "", v & 0x20u ? " 40-bit," : "",
                 v & 0x01000000u ? " stateless" : "");
    if (r < 0) { buf[0] = 0; return 0; }
    /* the list's last comma */
    {
        size_t len = (size_t)r < n ? (size_t)r : n - 1;
        char *c = strstr(buf, ",)");
        if (c) { memmove(c, c + 1, strlen(c + 1) + 1); len--; }
        return len;
    }
}

void srv_set_ppp_trace(srv_t *s, srv_ppp_trace_fn fn) { s->ppp_trace = fn; }

void srv_ppp_trace_text(int out, uint32_t ms, uint16_t proto, const uint8_t *pkt,
                        size_t pn, size_t len, char *buf, size_t n)
{
    static const char *const cp[16] = { "", "Configure-Request", "Configure-Ack", "Configure-Nak",
        "Configure-Reject", "Terminate-Request", "Terminate-Ack", "Code-Reject", "Protocol-Reject",
        "Echo-Request", "Echo-Reply", "Discard-Request", "Identification", "Time-Remaining",
        "Reset-Request", "Reset-Ack" };
    static const char *const pap[4] = { "", "Authenticate-Request", "Authenticate-Ack", "Authenticate-Nak" };
    static const char *const chap[5] = { "", "Challenge", "Response", "Success", "Failure" };
    const char *pn_ = proto == PPP_PROTO_LCP ? "LCP" : proto == PPP_PROTO_PAP ? "PAP"
                    : proto == PPP_PROTO_CHAP ? "CHAP" : proto == PPP_PROTO_IPCP ? "IPCP"
                    : proto == PPP_PROTO_CCP ? "CCP" : NULL;
    const char *cn = NULL;
    char pbuf[8], cbuf[16];
    size_t at, i;
    int r;

    if (!buf || !n) return;
    if (!pn_) { snprintf(pbuf, sizeof pbuf, "%04x", (unsigned)proto); pn_ = pbuf; }
    r = snprintf(buf, n, "PPP +%lu.%03lu s %s %s", (unsigned long)(ms / 1000u), (unsigned long)(ms % 1000u),
                 out ? "out" : "in ", pn_);
    at = r < 0 ? 0 : (size_t)r < n ? (size_t)r : n - 1;
    if (pn < 4) {
        snprintf(buf + at, n - at, " %lu octet%s", (unsigned long)len, len == 1 ? "" : "s");
        return;
    }
    if (proto == PPP_PROTO_PAP) cn = pkt[0] < 4 ? pap[pkt[0]] : NULL;
    else if (proto == PPP_PROTO_CHAP) cn = pkt[0] < 5 ? chap[pkt[0]] : NULL;
    else if (proto == PPP_PROTO_LCP || proto == PPP_PROTO_IPCP || proto == PPP_PROTO_CCP)
        cn = pkt[0] < 16 && pkt[0] && (proto == PPP_PROTO_CCP || pkt[0] < 14) ? cp[pkt[0]] : NULL;
    if (!cn || !*cn) { snprintf(cbuf, sizeof cbuf, "code %u", (unsigned)pkt[0]); cn = cbuf; }
    r = snprintf(buf + at, n - at, " %s id %u, %lu octets", cn, (unsigned)pkt[1], (unsigned long)len);
    at += r < 0 ? 0 : (size_t)r < n - at ? (size_t)r : n - at - 1;
    for (i = 4; i < pn && at + 4 < n; i++) {
        r = snprintf(buf + at, n - at, "%s%02x", i == 4 ? ": " : " ", (unsigned)pkt[i]);
        at += r < 0 ? 0 : (size_t)r < n - at ? (size_t)r : n - at - 1;
    }
    if (pn < len && pn > 4 && at + 4 < n)
        snprintf(buf + at, n - at, " ...");
}

size_t srv_vj_text(const ppp_counters_t *k, char *buf, size_t n)
{
    char to[80], from[80], drop[32] = "";
    int r;
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!k || (!k->vj_tx_slots && !k->vj_rx_slots)) return 0;
    if (k->vj_tx_slots)
        snprintf(to, sizeof to, "%lu of %lu TCP packets compressed (%u slots)", (unsigned long)k->vj_tx_comp,
                 (unsigned long)(k->vj_tx_comp + k->vj_tx_uncomp), (unsigned)k->vj_tx_slots);
    else snprintf(to, sizeof to, "not agreed");
    if (k->vj_rx_slots)
        snprintf(from, sizeof from, "%lu of %lu (%u slots)", (unsigned long)k->vj_rx_comp,
                 (unsigned long)(k->vj_rx_comp + k->vj_rx_uncomp), (unsigned)k->vj_rx_slots);
    else snprintf(from, sizeof from, "not agreed");
    if (k->vj_errors) snprintf(drop, sizeof drop, ", %lu dropped", (unsigned long)k->vj_errors);
    r = snprintf(buf, n, "VJ header compression: to caller %s, from caller %s%s", to, from, drop);
    return r < 0 ? 0 : (size_t)r < n ? (size_t)r : n - 1;
}

size_t srv_qc_text(const v90m_qc *q, char *buf, size_t n)
{
    char ask[32], lvl[16];
    int r, l;
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!q || !q->heard) return 0;
    if (q->uqts < 0)
        r = snprintf(buf, n, "V.92 Quick Connect: a request that is not one, ignored: V.8 instead");
    else {
        snprintf(ask, sizeof ask, "Ucode %d%s", q->uqts, q->lapm ? ", LAPM" : "");
        if (!q->accepted)
            r = snprintf(buf, n, "V.92 Quick Connect: asked (%s); not taken: V.8 instead", ask);
        else {
            l = q->level_db10 < 0 ? -q->level_db10 : q->level_db10;
            snprintf(lvl, sizeof lvl, "%s%d.%d", q->level_db10 < 0 ? "-" : "", l / 10, l % 10);
            if (q->fallback)
                r = snprintf(buf, n, "V.92 Quick Connect: asked (%s); taken, ANSpcm at %s dBm0, no TONEq within 2 s: V.8 instead", ask, lvl);
            else
                r = snprintf(buf, n, "V.92 Quick Connect: asked (%s); taken, ANSpcm at %s dBm0, TONEq after %d ms%s", ask, lvl, q->toneq_ms,
                             q->short_p2 == 1 ? ", short Phase 2" : q->short_p2 == 2 ? ", short Phase 2 begun, then the full one" : "");
        }
    }
    return r < 0 ? 0 : (size_t)r < n ? (size_t)r : n - 1;
}

size_t srv_k56_text(const v90m_k56 *k, char *buf, size_t n)
{
    static const char *const vers[4] = { "no versions", "version 1", "version 2", "versions 1 and 2" };
    int r;
    if (!buf || !n) return 0;
    buf[0] = 0;
    if (!k || (!k->offered && !k->exchanged)) return 0;
    if (!k->exchanged)
        r = snprintf(buf, n, "K56flex: we offered %s; the caller did not choose K56flex", vers[k->offered & 3]);
    else
        r = snprintf(buf, n, "K56flex: we offered %s; the caller chose version %d%s", vers[k->offered & 3], k->chosen,
                     k->declined ? ", declined: the call went on without K56flex" : k->agreed ? ", agreed" : "");
    return r < 0 ? 0 : (size_t)r < n ? (size_t)r : n - 1;
}
static const char *const webtv_numbers[] = { SRV_WEBTV, SRV_WEBTV_TF, SRV_WEBTV_56 };
/* a port for a WebTV box: every call on it is the box's */
static int webtv_here(const srv_t *s) { return s->cfg.service == CFG_SERVICE_WEBTV; }
/* a WebTV box logs in with its own built-in name and password: any is taken */
static int webtv_pap(void *ctx, const uint8_t *user, size_t ulen, const uint8_t *pass, size_t plen)
{
    (void)ctx; (void)user; (void)ulen; (void)pass; (void)plen;
    return 1;
}
/* the PROTOWEB number answers on this port */
static int protoweb_here(const srv_t *s)
{
    return s->protoweb_ok && (s->cfg.service == CFG_SERVICE_INTERNET || s->cfg.service == CFG_SERVICE_ARCHIVE);
}

/* the time service answers: the box knows the time (srv_set_clock) */
static int acts_here(const srv_t *s) { return s->clock >= 1600000000u; }

/* the telephony layer's question while digits come in (tel_cfg.more) */
static int tel_more(void *ctx, const char *dialed)
{
    const srv_t *s = ctx; const cfg_t *c = &s->cfg;
    int i, best = 0, m;
    /* a port that takes any number (tel.h): only a WebTV port's own numbers
     * end the dialling at once there; on the others it ends on the pause */
    if (webtv_here(s)) {
        for (i = 0; i < 3; i++) if ((m = cfg_svc_number_begins(webtv_numbers[i], dialed)) > best) best = m;
        return best;
    }
    if (c->service == CFG_SERVICE_SATURN || !c->line_number[0]) return 0;
    for (i = 0; i < c->n_services && i < CFG_MAX_SERVICES; i++)
        if ((m = cfg_svc_number_begins(c->services[i].number, dialed)) > best) best = m;
    if (best < 2 && c->svc_netstalgia && s->svc_known && (m = s->svc_known(dialed)) > best) best = m;
    if (best < 2 && protoweb_here(s) && (m = cfg_svc_number_begins(SRV_PROTOWEB, dialed)) > best) best = m;
    if (best < 2 && acts_here(s) && (m = cfg_svc_number_begins(SRV_ACTS, dialed)) > best) best = m;
    return best;
}

int srv_link(const srv_t *s, uint32_t *addr, int *online)
{
    if (!s->in_call || s->link == SATURN_NONE) { *online = 0; return SATURN_NONE; }
    *addr = s->link_addr;
    *online = s->dce.state == DCE_ONLINE;
    return s->link;
}

const char *srv_link_game(const srv_t *s) { return s->link_game; }

size_t srv_send_raw(srv_t *s, const uint8_t *oct, size_t n)
{
    size_t r;
    if (!s->in_call || s->link == SATURN_NONE) return 0;
    r = dce_raw_write(&s->dce, oct, n);
    if (r) { tel_activity(&s->tel); s->status.tx_octets += r; }
    return r;
}

void srv_ppp_watch(srv_t *s, int on)
{
    if (s->in_call && s->link == SRV_LINK_MENU) dce_ppp_watch(&s->dce, on);
}

int srv_start_ppp(srv_t *s)
{
    if (!s->in_call || s->link != SRV_LINK_MENU || dce_start_ppp(&s->dce) < 0) return -1;
    s->link = SATURN_NONE;
    s->ppp_from_s = s->call_s;
    return 0;
}

void srv_hangup(srv_t *s)
{
    if (s->in_call && !s->hangup_sent) { dce_hangup(&s->dce); s->hangup_sent = 1; }
}

static void ip_from_caller(void *ctx, const uint8_t *pkt, size_t len)
{
    srv_t *s = (srv_t *)ctx;
    tel_activity(&s->tel);
    s->status.rx_packets++; s->status.rx_octets += len;
    if (for_console(&s->cfg) && len <= sizeof s->pkt) {
        memcpy(s->pkt, pkt, len);
        if (!console_from_caller(&s->console, s->pkt, len, s->call_s * 1000u + (uint32_t)s->sub / 8u)) return;
        pkt = s->pkt;
    }
    if (s->ip_out) s->ip_out(s->ctx, pkt, len);
}


static void tel_settings(const srv_t *s, tel_cfg *t)
{
    const cfg_t *c = &s->cfg;
    memset(t, 0, sizeof *t);
    memcpy(t->number, c->line_number, TEL_NUMBER_MAX); t->number[TEL_NUMBER_MAX] = 0;
    /* any number: on Saturn what is dialled is an instruction (saturn.h), a
     * WebTV box's is the box's */
    if (c->service == CFG_SERVICE_SATURN || c->service == CFG_SERVICE_WEBTV) t->number[0] = 0;
    /* on every port: 1 800 BBS DIAL and the boards' own numbers (bbs.h) */
    strcpy(t->also[0], BBS_NUMBER); strcpy(t->also[1], BBS_NUMBER_LONG);
    strcpy(t->also[2], BBS_AREA "???????"); strcpy(t->also[3], "1" BBS_AREA "???????");
    t->tones = c->line_tones == CFG_TONES_EU ? TEL_TONES_EU : TEL_TONES_US;
    t->rings = c->rings;
    t->busy_chance = c->busy_chance;
    t->drop_mean_min = c->drop_mean_min;
    t->session_limit_min = c->session_limit_min;
    t->idle_timeout_min = c->idle_timeout_min;
    t->tone_level = srv_level_rms(-13) * 181 / 128;     /* each component -13 dBm0, as a peak */
    t->seed = s->seed;
    t->originate = s->originate;
    t->more = tel_more; t->more_ctx = (void *)s;
}

static void adopt(srv_t *s, const cfg_t *cfg)
{
    tel_cfg t; int i;
    s->cfg = *cfg; s->have_next = 0;
    s->status.service = cfg->service;
    s->status.v92 = cfg->v92 && full_options(cfg, 0);
    s->status.ppp_compression = cfg->compression && full_options(cfg, 0);
    /* MPPE for a caller that asks: a login offers MS-CHAPv2 first
     * (call_start), never on a console or WebTV port */
    s->status.ppp_encryption = cfg->auth != CFG_AUTH_NONE && full_options(cfg, 0) && cfg->service != CFG_SERVICE_WEBTV;
    for (i = 0; i < CFG_MAX_USERS; i++) {
        s->users[i].name = s->cfg.users[i].name;
        s->users[i].secret = s->cfg.users[i].secret;
    }
    tel_settings(s, &t);
    tel_reconfigure(&s->tel, &t);
}

void srv_init(srv_t *s, const cfg_t *cfg, uint32_t seed, srv_ip_fn ip_out, void *ctx)
{
    tel_cfg t;
    memset(s, 0, sizeof *s);
    s->seed = seed ? seed : 1; s->ip_out = ip_out; s->ctx = ctx;
    s->echo_delay = -1;                 /* measured by the modem */
    s->cfg = *cfg;
    tel_settings(s, &t);
    tel_init(&s->tel, &t);
    status_init(&s->status);
    adopt(s, cfg);                      /* after status_init: it reports the settings */
    s->last_tel = TEL_ONHOOK;
}

void srv_reconfigure(srv_t *s, const cfg_t *cfg)
{
    if (s->tel.state == TEL_ONHOOK) adopt(s, cfg);
    else { s->next = *cfg; s->have_next = 1; }
}

void srv_set_echo_delay(srv_t *s, int samples) { s->echo_delay = samples; }
void srv_set_rx_linear(srv_t *s, int on) { s->rx_linear = on != 0; }
void srv_set_originate(srv_t *s, int on)
{
    tel_cfg t;
    s->originate = on != 0;
    tel_settings(s, &t);
    tel_reconfigure(&s->tel, &t);
}
void srv_set_max_tx_code(srv_t *s, int code) { s->max_tx_code = code >= 1 && code <= 31 ? code : 0; }
void srv_set_heavy_slot(srv_t *s, int *slot) { s->heavy_slot = slot; }

/* give back the heavy-call slot this call reserved (see srv_set_heavy_slot) */
static void heavy_release(srv_t *s)
{
    if (s->heavy_held) {
        if (s->heavy_slot && *s->heavy_slot > 0) (*s->heavy_slot)--;
        s->heavy_held = 0;
    }
}

void srv_set_clock(srv_t *s, uint32_t utc_seconds) { s->clock = utc_seconds; }

int srv_ip_up(const srv_t *s)
{
    return s->in_call && s->dce.state == DCE_ONLINE && s->ds.ip_up;
}

int srv_send_ip(srv_t *s, const uint8_t *pkt, size_t len)
{
    int r;
    if (!s->in_call) return PPP_ERR_DOWN;
    r = dce_send_ip(&s->dce, pkt, len);
    if (r == 0) { tel_activity(&s->tel); s->status.tx_packets++; s->status.tx_octets += len; }
    else if (r != PPP_ERR_FULL) s->status.tx_errors++;
    return r;
}

/* the port's own number as the telephony layer takes it (tel.c, dialed_is):
 * a ten-digit one with the long distance 1 in front as well */
static int own_number(const cfg_t *c, const char *dialed)
{
    if (strcmp(dialed, c->line_number) == 0) return 1;
    return strlen(c->line_number) == 10 && c->line_number[0] != '1' && dialed[0] == '1' && strcmp(dialed + 1, c->line_number) == 0;
}

static void call_start(srv_t *s)
{
    dce_cfg_t d;
    const cfg_t *c = &s->cfg;
    int bbs, svc, acts, enc, webtv_tf, other; char board[BBS_NUMBER_LEN + 1];
    s->link = SATURN_NONE;
    s->call_start = s->clock; s->call_s = 0; s->ppp_up_once = 0; s->ppp_from_s = 0;
    /* a WebTV port: every call is the box's, whatever it dialled */
    s->webtv = webtv_here(s);
    /* a number of something else than the port's own service */
    other = !s->webtv && !own_number(c, s->tel.dialed);
    /* a terminal program for a bulletin board (bbs.h) */
    bbs = other && bbs_number(s->tel.dialed, board) != BBS_NUM_NONE;
    /* a number that connects straight to a service ([services]) */
    svc = other && !bbs && srv_svc_number(s, s->tel.dialed);
    /* the PROTOWEB number: the port's dial-up connection, with the archive */
    s->protoweb = other && !bbs && !svc && protoweb_here(s) && cfg_svc_number_match(SRV_PROTOWEB, s->tel.dialed);
    /* the toll-free number of a box with no TellyScript (srv.h) */
    webtv_tf = s->webtv && cfg_svc_number_match(SRV_WEBTV_TF, s->tel.dialed);
    /* the time service (SRV_ACTS): ACTS's own terms, octets at the line's rate */
    acts = other && !bbs && !svc && !s->protoweb && acts_here(s) && cfg_svc_number_match(SRV_ACTS, s->tel.dialed);
    if (bbs) s->link = SRV_LINK_BBS;
    else if (svc) s->link = SRV_LINK_SVC;
    else if (acts) s->link = SRV_LINK_ACTS;
    else if (for_link(c)) {
        s->link = saturn_number(s->tel.dialed, &s->link_addr, s->link_game);
        if ((s->link == SATURN_MATCH_WAIT || s->link == SATURN_MATCH_CALL) && !c->sat_match) s->link = SATURN_NONE;
        if (s->link == SATURN_NONE) {
            /* not a number of this service: no call. (The telephony layer
             * takes any number on such a port; it cannot know them.) */
            memset(&s->ds, 0, sizeof s->ds);
            s->ds.state = DCE_ENDED; s->ds.end_reason = DCE_END_LOCAL;
            s->in_call = 0;
            tel_call_ended(&s->tel);
            return;
        }
    }
    bbs |= svc;             /* from here on: a call that carries octets */
    dce_cfg_default(&d);
    d.tx_level = srv_level_rms(c->tx_level);
    d.modulations = 0;
    if (c->modulations & CFG_MOD_V90) d.modulations |= V90M_MOD_V90;
    /* V.92 rides on the V.90 offer. Only with error control: a V.92 caller
     * that saw LAPM in our JM skips ODP / ADP (V.92 9.3.1) and would wait
     * for a LAPM that v42 = off never starts */
    if ((c->modulations & CFG_MOD_V90) && c->v92 && c->v42 && full_options(c, bbs)) d.modulations |= V90M_MOD_V92;
    if (c->modulations & CFG_MOD_V34) d.modulations |= V90M_MOD_V34;
    if (c->modulations & CFG_MOD_V32BIS) d.modulations |= V90M_MOD_V32BIS;
    /* 1200 bit/s: V.22 and, behind a 2225 Hz answer tone, Bell 212A */
    if (c->modulations & (CFG_MOD_V22BIS | CFG_MOD_V22)) d.modulations |= V90M_MOD_V22 | V90M_MOD_BELL212A;
    /* 300 bit/s: V.21 and, for a caller that turns out to be one, Bell 103 */
    if (c->modulations & CFG_MOD_V21) d.modulations |= V90M_MOD_V21 | V90M_MOD_BELL103;
    /* x2 (U.S. Robotics' 56k of 1997) and K56flex (the 1996 56k, experimental):
     * each offered alongside V.34 when the owner turned it on for this line.
     * The library needs V.34 with either (without it V90M_ENOTSUP), offers
     * neither on an A-law line, and never in the calling role (both are
     * answering only). Either may be on for both ports, but only one heavy
     * call runs at a time: two heavy data-mode calls do not fit the CPU, so a
     * line offers x2 or K56flex only while the shared slot is free, and holds
     * it for this call. Only one modulation can connect per call, so a call
     * that offers both still needs just one slot. Reserved here (before the
     * originate strip below) only when we really will offer one, so the slot
     * never leaks. */
    if ((c->x2 || c->k56) && (c->modulations & CFG_MOD_V34) && !s->originate
        && full_options(c, bbs) && !acts && !webtv_tf
        && (!s->heavy_slot || *s->heavy_slot == 0)) {
        if (c->x2)  d.modulations |= V90M_MOD_X2;
        if (c->k56) d.modulations |= V90M_MOD_K56;
        s->heavy_held = 1;
        if (s->heavy_slot) (*s->heavy_slot)++;
    }
    d.alaw = c->law == CFG_LAW_ALAW;
    d.echo_delay = s->echo_delay;
    d.rx_linear = s->rx_linear; d.max_tx_code = s->max_tx_code;
    d.max_down = c->max_down; d.max_up = c->max_up;
    if (s->originate) {
        /* what the calling side can do so far */
        d.originate = 1;
        d.modulations &= V90M_MOD_V34 | V90M_MOD_V32BIS | V90M_MOD_V22 | V90M_MOD_V21;
    }
    if (webtv_tf) d.modulations &= ~(V90M_MOD_V90 | V90M_MOD_V92);      /* (x2 and K56flex: not offered above) */
    /* a WebTV box's modem asks V.34 for more than its own line probe
     * projected and then fails error control: our transmit kept to the
     * box's projection (libv90modem's V.34 transmit cap; answering only) */
    if (s->webtv) d.modulations |= V90M_OPT_V34_TX_CAP;
    if (!c->v42) { d.v42.detect = 1; d.v42.fallback = 1; d.v42.t400_ms = 1; }  /* straight to start-stop */
    if (acts) {
        /* ACTS's terms: up to 9600 bit/s and on (V.32bis here; no V.34 or
         * V.90, whose delay wanders more), 8N1 with no error control or
         * compression, so that an octet's time is its time on the line */
        uint32_t m = d.modulations & (V90M_MOD_V32BIS | V90M_MOD_V22 | V90M_MOD_BELL212A | V90M_MOD_V21 | V90M_MOD_BELL103);
        d.modulations = m ? m : d.modulations & V90M_MOD_V34;   /* the owner left only fast ones on */
        d.v42.detect = 1; d.v42.fallback = 1; d.v42.t400_ms = 1;
        d.v42.lapm.v42bis_tx = 0; d.v42.lapm.v42bis_rx = 0; d.v42.lapm.v44_tx = 0; d.v42.lapm.v44_rx = 0;
    }
    if (!c->v42bis) {
        /* the switch is for data compression as a whole: V.42 bis and V.44 */
        d.v42.lapm.v42bis_tx = 0; d.v42.lapm.v42bis_rx = 0;
        d.v42.lapm.v44_tx = 0; d.v42.lapm.v44_rx = 0;
    }
    /* Encryption on request: with a login (CHAP or PAP), MS-CHAPv2 is
     * offered first (MS-CHAP v1 first to Windows 98, recognised by its first
     * LCP request: it logs in with v2 but makes MPPE keys from v1 only), then
     * the other MS-CHAP, CHAP-MD5 and (a PAP port) PAP, each for a caller
     * that Naks or Rejects the one before; so a caller that wants
     * MPPE has the keys for it and gets it (ppp.h, encrypt 0), and one
     * that does not ask is served in the clear. Never on a console port,
     * which keeps the port's own login, nor for a WebTV box. */
    enc = c->auth != CFG_AUTH_NONE && full_options(c, bbs) && !s->webtv;
    if (s->webtv) {
        /* the box's own PAP login, whoever it says it is; some boxes give
         * up on a link that asks for none */
        d.ppp.auth = PPP_AUTH_PAP;
        d.ppp.pap_cb = webtv_pap;
        d.ppp.no_vj = 1;            /* no VJ header compression either: not tried with a box */
    } else if (enc)
        d.ppp.auth = PPP_AUTH_MSCHAPV1 | PPP_AUTH_MSCHAPV2 | PPP_AUTH_CHAP | (c->auth == CFG_AUTH_PAP ? PPP_AUTH_PAP : 0);
    else
        d.ppp.auth = c->auth == CFG_AUTH_CHAP ? PPP_AUTH_CHAP : c->auth == CFG_AUTH_PAP ? PPP_AUTH_PAP : PPP_AUTH_NONE;
    d.ppp.encrypt = 0;              /* on request, never required */
    d.ppp.users = s->users; d.ppp.n_users = (size_t)c->n_users;
    d.ppp.our_ip = c->ppp_local; d.ppp.peer_ip = c->ppp_peer;
    /* the callers' name server: the box (dnsrelay.h), or the servers themselves */
    if (c->dns_relay) { d.ppp.dns1 = c->ppp_local; d.ppp.dns2 = 0; }
    else { d.ppp.dns1 = c->dns[0]; d.ppp.dns2 = c->dns[1]; }
    d.ppp.mru = (uint16_t)c->mru;
    d.ppp.echo_interval_ms = (uint32_t)c->echo_interval * 1000u;
    /* [ppp] compression: CCP with Deflate and MPPC; off, CCP only for a
     * caller that asks for MPPE */
    d.ppp.comp = !c->compression ? PPP_COMP_NONE : (PPP_COMP_DEFLATE | PPP_COMP_MPPC);
    if (for_console(c) && !bbs) {
        /* The number and the login are the port's, as for every service.
         * Whatever [ppp] and [network] say: no CCP (one game is known to
         * get it wrong, and the console has no use for it), and the name
         * servers that know where the games' servers are today. V.92 is
         * left out above: the console's modem is older than it. */
        d.ppp.comp = PPP_COMP_NONE;
        d.ppp.no_vj = 1;            /* nor VJ header compression, for the same reason */
        d.ppp.dns1 = CONSOLE_DNS1; d.ppp.dns2 = CONSOLE_DNS2;
        console_init(&s->console);
    }
    /* a different seed for every call */
    s->seed = s->seed * 1664525u + 1013904223u;
    d.ppp.seed = s->seed ^ s->clock;
    if (s->entropy_n) { d.ppp.entropy = s->entropy; d.ppp.entropy_n = s->entropy_n; }
    d.ppp.ctx = s; d.ppp.ip_rx = ip_from_caller;
    d.ppp.trace = s->ppp_trace;
    if (for_link(c) || bbs || acts) d.raw_rx = raw_from_caller;
    d.raw_ec = bbs;
    /* The terminal menu on a PPP service's own number: the call starts as
     * octets with V.42 kept; a Dial-Up Networking caller's first PPP frame
     * takes it to PPP at once (dce.h, ppp_detect), anyone else gets the
     * menu, whose Start PPP does the same later (srv_start_ppp). */
    if (c->svc_menu && !bbs && !acts && !s->protoweb && !s->webtv && !s->originate && !for_console(c) && !for_link(c)) {
        s->link = SRV_LINK_MENU;
        d.raw_rx = raw_from_caller; d.raw_ec = 1; d.ppp_detect = 1;
    }
    dce_init(&s->dce, &d);
    dce_get_status(&s->dce, &s->ds);
    s->in_call = 1; s->hangup_sent = 0;
}

static st_mod_t mod_of(int m)
{
    switch (m) {
    case V90M_CONN_V90: return ST_MOD_V90;
    case V90M_CONN_V92: return ST_MOD_V92;
    case V90M_CONN_X2: return ST_MOD_X2;
    case V90M_CONN_K56FLEX: return ST_MOD_K56FLEX;
    case V90M_CONN_V34: return ST_MOD_V34;
    case V90M_CONN_V32BIS: return ST_MOD_V32BIS;
    case V90M_CONN_V22BIS: return ST_MOD_V22BIS;
    case V90M_CONN_V22: return ST_MOD_V22;
    case V90M_CONN_V21: return ST_MOD_V21;
    case V90M_CONN_BELL103: return ST_MOD_BELL103;
    case V90M_CONN_BELL212A: return ST_MOD_BELL212A;
    default: return ST_MOD_NONE;
    }
}

/* the live part of the status page */
static void refresh(srv_t *s)
{
    status_t *st = &s->status;
    const dce_status_t *d = &s->ds;
    switch (s->tel.state) {
    case TEL_ONHOOK: st->line = ST_LINE_IDLE; break;
    case TEL_ANSWERED: st->line = s->in_call && d->modem_connected ? ST_LINE_CONNECTED : ST_LINE_TRAINING; break;
    case TEL_ENDED: st->line = ST_LINE_HANGUP; break;
    default: st->line = ST_LINE_RINGING; break;         /* dialling, ringing tone, busy and the like */
    }
    if (!s->in_call) {
        st->modulation = ST_MOD_NONE; st->down_bps = st->up_bps = 0;
        st->v42 = ST_V42_OFF; st->compression = 0; st->ppp = ST_PPP_DEAD;
        st->peer_name[0] = 0; st->peer_ip = 0; st->local_ip = 0;
        st->ppp_comp_tx = st->ppp_comp_rx = ST_COMP_NONE;
        st->comp_tx_in = st->comp_tx_out = st->comp_rx_in = st->comp_rx_out = 0;
        st->rtd_ms = 0; st->rx_level_x10 = 0; st->snr_x10 = 0;
        return;
    }
    st->modulation = d->modem_connected ? mod_of(d->modulation) : ST_MOD_NONE;
    st->down_bps = d->modem_connected ? (uint32_t)d->bitrate : 0;
    st->up_bps = d->modem_connected ? (uint32_t)d->rx_bitrate : 0;
    st->snr_x10 = d->modem_connected && d->snr_db10 > 0 ? d->snr_db10 : 0;
    st->rtd_ms = d->modem_connected && d->rtd > 0 ? (uint32_t)(d->rtd + 4) / 8 : 0;
    /* what came off the line in the last whole second, the echo of our own
     * signal included, against the 0 dBm0 sine */
    st->rx_level_x10 = d->modem_connected && s->rx_pow_last > 0 ?
        db10(s->rx_pow_last, (int64_t)RMS_0DBM0 * RMS_0DBM0) : 0;
    st->v42 = !d->modem_connected ? ST_V42_OFF :
              d->error_control ? ST_V42_LAPM :
              d->v42_state == V90M_EC_ASYNC ? ST_V42_ASYNC : ST_V42_DETECT;
    st->compression = d->comp_tx || d->comp_rx;
    switch (d->ppp_phase) {
    case PPP_PHASE_ESTABLISH: st->ppp = ST_PPP_ESTABLISH; break;
    case PPP_PHASE_AUTHENTICATE: st->ppp = ST_PPP_AUTHENTICATE; break;
    case PPP_PHASE_NETWORK: st->ppp = d->ip_up ? ST_PPP_RUNNING : ST_PPP_NETWORK; break;
    case PPP_PHASE_TERMINATE: st->ppp = ST_PPP_TERMINATE; break;
    default: st->ppp = ST_PPP_DEAD; break;
    }
    memcpy(st->peer_name, d->peer_name, STATUS_NAME_MAX); st->peer_name[STATUS_NAME_MAX] = 0;
    st->peer_ip = d->ip_up ? d->peer_ip : 0;
    st->local_ip = d->ip_up ? d->our_ip : 0;
    /* PPP compression: what is agreed now, and what went through it in this call */
    st->ppp_comp_tx = (int)d->ppp_comp_tx; st->ppp_comp_rx = (int)d->ppp_comp_rx;
    st->comp_tx_in = d->ppp_counters.comp_tx_in; st->comp_tx_out = d->ppp_counters.comp_tx_out;
    st->comp_rx_in = d->ppp_counters.comp_rx_in; st->comp_rx_out = d->ppp_counters.comp_rx_out;
}

/* the line is back on hook: one line of history for what happened */
static void episode_end(srv_t *s)
{
    st_call_t h;
    const dce_status_t *d = &s->ds;
    memset(&h, 0, sizeof h);
    memcpy(h.dialed, s->tel.dialed, STATUS_DIALED_MAX); h.dialed[STATUS_DIALED_MAX] = 0;
    if (s->tel.answered_this) {
        h.start_time = s->call_start;
        h.duration_s = s->call_s;
        h.modulation = mod_of(d->modulation);
        h.down_bps = (uint32_t)d->bitrate; h.up_bps = (uint32_t)d->rx_bitrate;
        switch (s->tel.end) {
        case TEL_END_DROP:  h.tel_result = ST_TEL_DROPPED; h.end = ST_END_LOCAL; break;
        case TEL_END_LIMIT: h.tel_result = ST_TEL_SESSION_LIMIT; h.end = ST_END_LOCAL; break;
        case TEL_END_IDLE:  h.tel_result = ST_TEL_IDLE; h.end = ST_END_LOCAL; break;
        case TEL_END_CALLER: h.tel_result = ST_TEL_ANSWERED; h.end = ST_END_CALLER_HANGUP; break;
        default:
            h.tel_result = ST_TEL_ANSWERED;
            switch (d->end_reason) {
            case DCE_END_NO_CARRIER: h.end = ST_END_TRAIN_FAILED; break;
            case DCE_END_CARRIER_LOST: h.end = ST_END_CARRIER_LOST; break;
            case DCE_END_RETRAIN_FAILED: h.end = ST_END_RETRAIN_FAILED; break;
            case DCE_END_AUTH_FAILED: h.end = ST_END_AUTH_FAILED; break;
            case DCE_END_PEER_CLOSED: case DCE_END_V42_RELEASED: h.end = ST_END_PPP_CLOSED; break;
            case DCE_END_PPP_FAILED: h.end = d->ppp_end == PPP_END_KEEPALIVE ? ST_END_ECHO_TIMEOUT : ST_END_ERROR; break;
            case DCE_END_LOCAL: h.end = ST_END_LOCAL; break;
            case DCE_END_CLEARDOWN: h.end = ST_END_CALLER_HANGUP; break;
            default: h.end = ST_END_ERROR; break;
            }
            break;
        }
    } else {
        h.start_time = s->clock;
        h.tel_result = s->tel.was_busy ? ST_TEL_BUSY : s->tel.was_wrong ? ST_TEL_WRONG_NUMBER : ST_TEL_ABANDONED;
        h.end = ST_END_NONE;
    }
    status_add_call(&s->status, &h);
}

uint8_t srv_tick(srv_t *s, int offhook, uint8_t rx)
{
    return srv_tick_lin(s, offhook, v90m_g711_decode(s->cfg.law == CFG_LAW_ALAW, rx));
}

uint8_t srv_tick_lin(srv_t *s, int offhook, int16_t lin)
{
    int alaw = s->cfg.law == CFG_LAW_ALAW;
    int16_t tone = tel_tick(&s->tel, offhook, lin);
    tel_state ts = s->tel.state;
    uint8_t out;

    if (ts != s->last_tel) {
        if (ts == TEL_ANSWERED) call_start(s);
        else if (s->last_tel == TEL_ANSWERED && s->in_call) {
            /* dropped, or the caller hung up: the call object is abandoned as it is */
            dce_get_status(&s->dce, &s->ds);
            heavy_release(s);
            s->in_call = 0;
        }
        if (ts != TEL_ONHOOK) s->episode = 1;
        if (ts == TEL_ONHOOK && s->episode) {
            episode_end(s);
            s->episode = 0;
            if (s->have_next) adopt(s, &s->next);
        }
        s->last_tel = ts;
        refresh(s);
    }

    if (s->in_call) {
        if (s->tel.want_hangup && !s->hangup_sent) { dce_hangup(&s->dce); s->hangup_sent = 1; }
        if (!dce_tick_lin(&s->dce, lin, &out)) {
            dce_get_status(&s->dce, &s->ds);
            heavy_release(s);
            s->in_call = 0;
            tel_call_ended(&s->tel);
            s->last_tel = s->tel.state;
            refresh(s);
        }
    } else {
        out = tone ? v90m_g711_encode(alaw, tone) : silence(s);
    }

    s->rx_pow += (int32_t)lin * lin;
    if (++s->sub >= 8000) {
        s->sub = 0;
        s->rx_pow_last = s->rx_pow / 8000; s->rx_pow = 0;
        s->status.uptime_s++;
        if (s->clock) s->clock++;
        if (s->in_call) {
            s->call_s++; dce_get_status(&s->dce, &s->ds);
            /* A PPP caller has SRV_LOGIN_MAX_S from the answer to be logged
             * in and have its network up, once: renegotiating LCP or
             * answering every request with a Nak does not hold a port. */
            if (s->ds.ppp_phase == PPP_PHASE_NETWORK) s->ppp_up_once = 1;
            /* the menu's caller went over to PPP by itself (its first frame) */
            if (s->link == SRV_LINK_MENU && s->ds.state == DCE_ONLINE && !s->ds.raw) {
                s->link = SATURN_NONE; s->ppp_from_s = s->call_s;
            }
            if (s->link == SATURN_NONE && !s->originate && !s->ppp_up_once && s->call_s - s->ppp_from_s >= SRV_LOGIN_MAX_S)
                srv_hangup(s);
        }
        refresh(s);
    }
    return out;
}
