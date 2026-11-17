/* Test peer: V.32bis calling modem in double. See v32call.h. */
#include <math.h>
#include <string.h>
#include "v32call.h"
#include "v32spec.h"

#define PI  3.14159265358979323846
#define SPS (10.0 / 3)
#define WC  (2 * PI * 1800 / 8000)

/* ======================= coding, from the Recommendation ======================= */

/* 4: scr[k] = the scrambled bit k bits ago. Call: 1 + x^-18 + x^-23, answer: 1 + x^-5 + x^-23 */
static int scramble(int *scr, int answer, int in)
{
    int out = in ^ scr[answer ? 5 : 18] ^ scr[23], k;
    for (k = 23; k > 1; k--) scr[k] = scr[k - 1];
    scr[1] = out;
    return out;
}

static int descramble(int *scr, int answer, int in)
{
    int out = in ^ scr[answer ? 5 : 18] ^ scr[23], k;
    for (k = 23; k > 1; k--) scr[k] = scr[k - 1];
    scr[1] = in;
    return out;
}

/* Tables 1 and 2, looked up in the transcribed rows */
static void table_lookup(const char *const *tab, int q1, int q2, int *y1, int *y2)
{
    int i;
    for (i = 0; i < 16; i++)
        if (tab[i][0] - '0' == q1 && tab[i][1] - '0' == q2 && tab[i][2] - '0' == *y1 && tab[i][3] - '0' == *y2) {
            *y1 = tab[i][5] - '0'; *y2 = tab[i][6] - '0';
            return;
        }
}

/* and backwards: which Q1 Q2 leads from (py1, py2) to (y1, y2) */
static int table_inverse(const char *const *tab, int py1, int py2, int y1, int y2)
{
    int i;
    for (i = 0; i < 16; i++)
        if (tab[i][2] - '0' == py1 && tab[i][3] - '0' == py2 && tab[i][5] - '0' == y1 && tab[i][6] - '0' == y2)
            return (tab[i][0] - '0') << 1 | (tab[i][1] - '0');
    return 0;
}

/* Figure 1, gate by gate. Delay elements ta, tb, tc from left to right;
 * returns Y0 (the output of the last one) and advances them. */
static int trellis(int *ta, int *tb, int *tc, int y1, int y2)
{
    int y0 = *tc;
    int top = y1 ^ y2;                 /* adder fed by Y1 and Y2 */
    int third = *tb ^ y2;              /* adder after the middle delay, fed by Y2 */
    int gate1 = y0 & third;            /* left AND: Y0 and that adder */
    int gate2 = y1 & y0;               /* right AND: Y1 and Y0 */
    int nb = *ta ^ top ^ gate1;
    int nc = third ^ gate2;
    *ta = y0; *tb = nb; *tc = nc;
    return y0;
}

static const v32spec_row *spec_rows(int rate)
{
    switch (rate) {
    case 0: return v32spec_4800;
    case 1: return v32spec_7200;
    case 2: return v32spec_9600;
    case 3: return v32spec_12000;
    default: return v32spec_14400;
    }
}

static void load_points(double pts[5][VC_NPTS][2])
{
    int rate;
    for (rate = 0; rate < 5; rate++) {
        const v32spec_row *row = spec_rows(rate);
        int nb = rate == 0 ? 2 : 3 + rate, sc = rate >= 3 ? 1 : 2, i;
        for (; row->labels; row++) {
            const char *p = row->labels;
            int re = row->re0;
            while (*p) {
                int label = 0;
                for (i = 0; i < nb; i++) label = (label << 1) | (*p++ - '0');
                if (*p == ' ') p++;
                pts[rate][label][0] = re * sc; pts[rate][label][1] = row->im * sc;
                re += row->step;
            }
        }
    }
}

/* signal state A..D (0..3): Figure 2-5 labels 00, 01, 11, 10 */
static const int state_label[4] = { 0, 1, 3, 2 };

/* ======================= pulses ======================= */
static double rrc(double t, double a)
{
    if (fabs(t) < 1e-9) return 1 - a + 4 * a / PI;
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / sqrt(2.0) * ((1 + 2 / PI) * sin(PI / (4 * a)) + (1 - 2 / PI) * cos(PI / (4 * a)));
    return (sin(PI * t * (1 - a)) + 4 * a * t * cos(PI * t * (1 + a))) / (PI * t * (1 - 16 * a * a * t * t));
}

static double bessel0(double x)
{
    double s = 1, t = 1; int k;
    for (k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}

static double kaiser(double u, double beta)
{
    return fabs(u) < 1 ? bessel0(beta * sqrt(1 - u * u)) / bessel0(beta) : 0;
}

#define VC_ALPHA 0.2

/* ======================= transmitter ======================= */
void vc_tx_init(vc_tx *t, int answer, double level, int (*getbit)(void *), void *user)
{
    int p, j;
    double pw = 0, pww = 0;
    memset(t, 0, sizeof *t);
    t->answer = answer; t->level = level;
    t->mode = VC_SILENCE;
    t->acc = 10;
    t->getbit = getbit; t->user = user;
    load_points(t->pts);
    for (p = 0; p < 10; p++)
        for (j = 0; j < VC_TXL; j++) {
            double tau = p / 3.0 + j * SPS - VC_TXL * SPS / 2, u = tau / (VC_TXL * SPS / 2);
            double x = 2 * 1800.0 * tau / 8000;
            t->g[p][j] = rrc(tau / SPS, VC_ALPHA) * kaiser(u, 2.5);
            t->w[p][j] = (fabs(x) < 1e-12 ? 1 : sin(PI * x) / (PI * x)) * kaiser(u, 4.5);
            pw += t->g[p][j] * t->g[p][j]; pww += t->w[p][j] * t->w[p][j];
        }
    /* line power for mean |a|^2 = 40 is 20 . sum(g^2) / 10 */
    for (p = 0; p < 10; p++)
        for (j = 0; j < VC_TXL; j++) {
            t->g[p][j] *= level / sqrt(2 * pw);
            t->w[p][j] *= level / sqrt(2 * pww);
        }
}

int vc_tx_symbol_due(const vc_tx *t) { return t->acc >= 10; }

void vc_tx_set(vc_tx *t, vc_txmode mode)
{
    if (mode != t->mode) {
        if (!(mode == VC_DATA && t->mode == VC_ONES)) t->nsym = 0;
        t->mode = mode;
    }
}

static void tx_symbol(vc_tx *t)
{
    double x = 0, y = 0;
    int i, s = -1, d1, d2, q[6] = { 0, 0, 0, 0, 0, 0 }, n;
    for (i = VC_TXL + VC_TXD - 1; i > 0; i--) { t->ar[i] = t->ar[i - 1]; t->ai[i] = t->ai[i - 1]; }
    switch (t->mode) {
    case VC_SILENCE: break;
    case VC_AA: s = 0; break;
    case VC_CC: s = 2; break;
    case VC_AC: s = (t->nsym & 1) ? 2 : 0; break;
    case VC_CA: s = (t->nsym & 1) ? 0 : 2; break;
    case VC_S:    s = (t->nsym & 1) ? 1 : 0; break;
    case VC_SBAR: s = (t->nsym & 1) ? 3 : 2; break;
    case VC_TRN:
        if (t->nsym == 0) memset(t->scr, 0, sizeof t->scr);
        d1 = scramble(t->scr, t->answer, 1); d2 = scramble(t->scr, t->answer, 1);
        if (t->nsym < 256) s = d1 ? 2 : 0;                    /* 5.2.3 */
        else s = (d1 == 0 && d2 == 0) ? 0 : (d1 == 0) ? 1 : (d2 == 1) ? 2 : 3;   /* Table 4 */
        break;
    case VC_RATE:
        if (t->nsym == 0) t->rbit = 0;
        d1 = scramble(t->scr, t->answer, (t->rword >> (15 - t->rbit)) & 1);
        d2 = scramble(t->scr, t->answer, (t->rword >> (14 - t->rbit)) & 1);
        t->rbit = (t->rbit + 2) & 15;
        table_lookup(v32spec_table2, d1, d2, &t->y1, &t->y2);
        for (s = 0; s < 4; s++) if (state_label[s] == (t->y1 << 1 | t->y2)) break;
        break;
    case VC_ONES:
    case VC_DATA:
        if (t->nsym == 0) t->ta = t->tb = t->tc = 0;          /* 6.1: delay elements to zero */
        n = 2 + t->rate;
        for (i = 0; i < n; i++)
            q[i] = scramble(t->scr, t->answer, t->mode == VC_DATA ? (t->getbit(t->user) & 1) : 1);
        if (t->rate == 0) {
            table_lookup(v32spec_table2, q[0], q[1], &t->y1, &t->y2);
            for (s = 0; s < 4; s++) if (state_label[s] == (t->y1 << 1 | t->y2)) break;
        } else {
            int label, y0;
            table_lookup(v32spec_table1, q[0], q[1], &t->y1, &t->y2);
            y0 = trellis(&t->ta, &t->tb, &t->tc, t->y1, t->y2);
            label = (y0 << 2) | (t->y1 << 1) | t->y2;
            for (i = 2; i < n; i++) label = (label << 1) | q[i];
            x = t->pts[t->rate][label][0]; y = t->pts[t->rate][label][1];
        }
        break;
    }
    if (s >= 0) {
        x = t->pts[0][state_label[s]][0]; y = t->pts[0][state_label[s]][1];
        t->y1 = state_label[s] >> 1; t->y2 = state_label[s] & 1;
    }
    t->ar[0] = x; t->ai[0] = y;
    t->nsym++; t->nsym_total++;
}

double vc_tx_tick(vc_tx *t)
{
    double bi = 0, bq = 0, ri = 0, rq = 0, c, s;
    int j;
    t->newsym = 0;
    if (t->acc >= 10) { t->acc -= 10; tx_symbol(t); t->newsym = 1; }
    for (j = 0; j < VC_TXL; j++) {
        bi += t->ar[j + VC_TXD] * t->g[t->acc][j]; bq += t->ai[j + VC_TXD] * t->g[t->acc][j];
        ri += t->ar[j] * t->w[t->acc][j];          rq += t->ai[j] * t->w[t->acc][j];
    }
    c = cos(WC * (double)(t->nsamp % 40)); s = sin(WC * (double)(t->nsamp % 40));
    t->ref = ri * c - rq * s;
    t->nsamp++;
    t->acc += 3;
    return bi * c - bq * s;
}

/* ======================= receiver ======================= */
/* detection latencies, samples, from the arrival of the reversed symbol's
 * pulse centre at the line to the detector firing (measured, test_startup) */
#define VC_REV_LAT 20.0

static void rx_reset_train(v32call_t *c)
{
    int i;
    for (i = 0; i < VC_EQN; i++) c->xr[i] = c->xi[i] = c->wr[i] = c->wi[i] = 0;
    c->theta = c->freq = 0; c->t_integ = 0;
    c->gain = 0; c->agc_acc = 0; c->agc_n = 0;
    c->o1r = c->o1i = c->o2r = c->o2i = 0; c->sbar_neg = 0;
    c->rsym = 0; c->ref_idx = 0;
    memset(c->ref_scr, 0, sizeof c->ref_scr); memset(c->dscr, 0, sizeof c->dscr);
    c->bits = 0; c->nbits = 0; c->frame_bit = -1;
    c->rate_word = 0; c->rate_seen = 0; c->e_word = 0;
    c->mse = 0;
}

static void vit_init(v32call_t *c)
{
    int s, y;
    for (s = 0; s < 8; s++) {
        c->pm[s] = s ? 1e9 : 0;
        for (y = 0; y < 4; y++) {
            int ta = (s >> 2) & 1, tb = (s >> 1) & 1, tc = s & 1;
            c->vy0[s] = trellis(&ta, &tb, &tc, y >> 1, y & 1);
            c->vnext[s][y] = ta << 2 | tb << 1 | tc;
        }
    }
    c->vpos = 0; c->vn = 0;
}

/* returns the label decided VC_VDEPTH symbols ago or -1; *hard = nearest label */
static int vit_step(v32call_t *c, double zr, double zi, int *hard)
{
    int rate = c->rx_rate, npts = 1 << (3 + rate), i, s, y, bl[8], best = 0;
    double bm[8], nm[8], min;
    for (i = 0; i < 8; i++) { bm[i] = 1e18; bl[i] = 0; }
    for (i = 0; i < npts; i++) {
        double dx = zr - c->tx.pts[rate][i][0], dy = zi - c->tx.pts[rate][i][1], d = dx * dx + dy * dy;
        int k = i >> rate;
        if (d < bm[k]) { bm[k] = d; bl[k] = i; }
    }
    for (i = 1; i < 8; i++) if (bm[i] < bm[best]) best = i;
    *hard = bl[best];
    for (s = 0; s < 8; s++) nm[s] = 1e18;
    for (s = 0; s < 8; s++)
        for (y = 0; y < 4; y++) {
            int ns = c->vnext[s][y], sub = c->vy0[s] << 2 | y;
            double m = c->pm[s] + bm[sub];
            if (m < nm[ns]) { nm[ns] = m; c->vh[c->vpos][ns] = bl[sub] << 3 | s; }
        }
    min = nm[0]; best = 0;
    for (s = 1; s < 8; s++) if (nm[s] < min) { min = nm[s]; best = s; }
    for (s = 0; s < 8; s++) c->pm[s] = nm[s] - min;
    c->vpos = (c->vpos + 1) & 31;
    if (++c->vn < VC_VDEPTH) return -1;
    s = best;
    for (i = 1; i < VC_VDEPTH; i++) s = c->vh[(c->vpos - i) & 31][s] & 7;
    return c->vh[(c->vpos - VC_VDEPTH) & 31][s] >> 3;
}

static void rx_frame(v32call_t *c, unsigned w)
{
    if ((w & 0xF111u) == 0xF111u) {
        int k, rate = -1;
        static const unsigned bit[5] = { 1u << 10, 1u << 6, 1u << 9, 1u << 5, 1u << 3 };   /* B5 B9 B6 B10 B12 */
        for (k = 0; k < 5; k++) if (w & bit[k]) rate = k;
        c->e_word = w;
        if (rate < 0) return;
        c->rx_rate = rate;
        vit_init(c);
        c->rxmode = 4;
        c->clamp = 128;
    } else if ((w & 0xF111u) == 0x0111u) {
        if (w == c->rate_word) c->rate_seen++;
        else { c->rate_word = w; c->rate_seen = 1; }
    }
}

static int rx_slice4(double zr, double zi)
{
    double a = 2 * zr - zi, b = 2 * zi + zr;       /* (2 + j) z: A..D into the quadrants */
    if (a >= 0) return b >= 0 ? 2 : 1;
    return b >= 0 ? 3 : 0;
}

static int rx_ref_state(v32call_t *c)
{
    int idx = c->ref_idx++, d1, d2;
    if (idx < 0) return idx & 1;
    if (idx < 16) return 2 + (idx & 1);
    if (idx == 16) memset(c->ref_scr, 0, sizeof c->ref_scr);
    d1 = scramble(c->ref_scr, 1, 1); d2 = scramble(c->ref_scr, 1, 1);     /* the answerer's scrambler */
    if (idx - 16 < 256) return d1 ? 2 : 0;
    return (d1 == 0 && d2 == 0) ? 0 : (d1 == 0) ? 1 : (d2 == 1) ? 2 : 3;
}

static void rx_symbol(v32call_t *c)
{
    double zr = 0, zi = 0, cs, sn, pr, pi_, dx, dy, er, ei, fr, fi, perr, mu, kp, kf;
    int k, st = -1, out = -1, i;

    for (k = 0; k < VC_EQN; k++) {
        zr += c->wr[k] * c->xr[k] - c->wi[k] * c->xi[k];
        zi += c->wr[k] * c->xi[k] + c->wi[k] * c->xr[k];
    }
    cs = cos(c->theta); sn = sin(c->theta);
    pr = zr * cs + zi * sn; pi_ = zi * cs - zr * sn;
    c->zr = pr; c->zi = pi_;

    if (c->rxmode == 2) {
        st = rx_ref_state(c);
        dx = c->tx.pts[0][state_label[st]][0]; dy = c->tx.pts[0][state_label[st]][1];
        mu = 0.3; kp = 0.125; kf = 0.002;
    } else if (c->rxmode == 3 || c->rx_rate == 0) {
        st = rx_slice4(pr, pi_);
        dx = c->tx.pts[0][state_label[st]][0]; dy = c->tx.pts[0][state_label[st]][1];
        mu = c->rxmode == 3 ? 0.08 : 0.02; kp = c->rxmode == 3 ? 0.125 : 0.03; kf = c->rxmode == 3 ? 0.002 : 1e-4;
        if (c->rxmode == 4) out = state_label[st];
    } else {
        int hard;
        out = vit_step(c, pr, pi_, &hard);
        dx = c->tx.pts[c->rx_rate][hard][0]; dy = c->tx.pts[c->rx_rate][hard][1];
        mu = 0.02; kp = 0.03; kf = 1e-4;
    }
    er = pr - dx; ei = pi_ - dy;
    c->mse += (er * er + ei * ei - c->mse) / 32;
    perr = (pi_ * dx - pr * dy) / (dx * dx + dy * dy);
    if (perr > 0.5) perr = 0.5; else if (perr < -0.5) perr = -0.5;
    c->freq += kf * perr;
    c->theta += kp * perr + c->freq;
    if (c->theta > PI) c->theta -= 2 * PI; else if (c->theta < -PI) c->theta += 2 * PI;
    fr = er * cs - ei * sn; fi = ei * cs + er * sn;
    mu /= VC_EQN * 40.0;
    for (k = 0; k < VC_EQN; k++) {
        c->wr[k] -= mu * (fr * c->xr[k] + fi * c->xi[k]);
        c->wi[k] -= mu * (fi * c->xr[k] - fr * c->xi[k]);
    }
    c->rsym++;

    if (c->rxmode == 2) {
        c->py1 = state_label[st] >> 1; c->py2 = state_label[st] & 1;
        if (c->ref_idx >= 16 + 1024) {
            c->rxmode = 3; c->rsym = 0;
            memcpy(c->dscr, c->ref_scr, sizeof c->dscr);
            c->bits = 0; c->nbits = 0; c->frame_bit = -1;
        }
    } else if (c->rxmode == 3) {
        int y1 = state_label[st] >> 1, y2 = state_label[st] & 1;
        int q = table_inverse(v32spec_table2, c->py1, c->py2, y1, y2);
        c->py1 = y1; c->py2 = y2;
        c->bits = (c->bits << 1) | (uint64_t)descramble(c->dscr, 1, q >> 1);
        c->bits = (c->bits << 1) | (uint64_t)descramble(c->dscr, 1, q & 1);
        if (c->nbits < 64) c->nbits += 2;
        if (c->frame_bit < 0) {
            unsigned w0 = (unsigned)(c->bits & 0xFFFF), w1 = (unsigned)((c->bits >> 16) & 0xFFFF),
                     w2 = (unsigned)((c->bits >> 32) & 0xFFFF);
            if (c->nbits >= 48 && w0 == w1 && (w0 & 0xF111u) == 0x0111u && (w2 & 0xF111u) == 0x0111u) {
                c->rate_word = w0; c->rate_seen = 2; c->frame_bit = 0;
            }
        } else {
            c->frame_bit += 2;
            if (c->frame_bit >= 16) { c->frame_bit = 0; rx_frame(c, (unsigned)(c->bits & 0xFFFF)); }
        }
    } else if (out >= 0) {
        int n = 2 + c->rx_rate, y1, y2, q;
        if (c->rx_rate == 0) { y1 = out >> 1; y2 = out & 1; q = table_inverse(v32spec_table2, c->py1, c->py2, y1, y2); }
        else {
            /* label: Y0 Y1 Y2 Q3 .. */
            y1 = (out >> (n - 1)) & 1; y2 = (out >> (n - 2)) & 1;
            q = table_inverse(v32spec_table1, c->py1, c->py2, y1, y2);
        }
        c->py1 = y1; c->py2 = y2;
        for (i = 0; i < n; i++) {
            int in = i == 0 ? q >> 1 : i == 1 ? q & 1 : (out >> (n - 1 - i)) & 1;
            int b = descramble(c->dscr, 1, in);
            if (!c->clamp && c->cfg.putbit) c->cfg.putbit(c->cfg.user, b);
        }
        if (c->clamp) c->clamp--;
    }
}

static void rx_timing(v32call_t *c, double p)
{
    double qr = c->ur * c->lr + c->ui * c->li, qi = c->ui * c->lr - c->ur * c->li;
    double a = -2 * PI * 0.3 * p, xr = qr * cos(a) - qi * sin(a), xi = qi * cos(a) + qr * sin(a), e;
    if (fabs(xr) + fabs(xi) < 1e-9) return;
    e = atan2(xi, xr) / (2 * PI);                    /* cycles of the symbol clock */
    if (c->rxmode == 1) c->tnext -= 0.3 * e * SPS;
    else {
        c->t_integ += 1e-4 * e * SPS;
        c->tnext -= 0.02 * e * SPS + c->t_integ;
    }
}

static void rx_half(v32call_t *c, double yr, double yi, double p)
{
    double pw = yr * yr + yi * yi, ar, ai, br, bi, m, o4r, o4i, vr = 0, vi = 0, tq, gr, gi;
    int k, ac, sp;

    c->pw += (pw - c->pw) / 16;
    o4r = c->yr[(c->ypos - 3) & 7]; o4i = c->yi[(c->ypos - 3) & 7];
    c->ypos = (c->ypos + 1) & 7;
    c->yr[c->ypos] = yr; c->yi[c->ypos] = yi;
    /* the alternating part: half the difference of samples a symbol apart;
     * steady over two symbols for AC (600 + 3000 Hz), and our own AA / CC
     * echo cancels in it */
    ar = (yr - c->yr[(c->ypos - 2) & 7]) / 2; ai = (yi - c->yi[(c->ypos - 2) & 7]) / 2;
    br = (c->yr[(c->ypos - 4) & 7] - c->yr[(c->ypos - 6) & 7]) / 2; bi = (c->yi[(c->ypos - 4) & 7] - c->yi[(c->ypos - 6) & 7]) / 2;
    m = ar * br + ai * bi;
    for (k = 0; k < 4; k++) { vr += c->yr[(c->ypos - k) & 7] / 4; vi += c->yi[(c->ypos - k) & 7] / 4; }
    tq = vr * vr + vi * vi;                          /* the 1800 Hz part */
    if (c->rev_hold > 0) c->rev_hold--;
    else {
        if (c->ac_run >= 32 && m < -0.4 * c->ac_p) {
            c->rev_count++; c->rev_t = c->nsamp; c->rev_hold = 10;
        } else {
            c->ac_m += (m - c->ac_m) / 8;
            c->ac_p += (ar * ar + ai * ai - c->ac_p) / 8;
        }
        ac = c->ac_p > 100.0 && c->ac_m > 0.6 * c->ac_p && (c->state == VCS_AA || c->state == VCS_CC || c->ac_p > 0.6 * c->pw);
        /* (not straight to zero: the reversal itself, sampled half way through,
         * can spoil the ratio one sample before the product goes negative) */
        if (ac) c->ac_run++; else c->ac_run = c->ac_run > 8 ? c->ac_run - 8 : 0;
    }
    c->c4 += (yr * o4r + yi * o4i - c->c4) / 16;
    /* S: periodic over two symbols, with both an 1800 Hz and an alternating part */
    c->tqs += (tq - c->tqs) / 16;
    sp = c->pw > 100.0 && c->c4 > 0.6 * c->pw && c->tqs > 0.25 * c->pw && c->tqs < 0.9 * c->pw;
    if (sp) c->s_run++; else c->s_run = 0;

    if (c->rxmode == 0) { c->half ^= 1; return; }
    if (c->gain == 0) {
        if (c->agc_n >= 16) c->agc_acc += pw;
        if (++c->agc_n == 48) c->gain = sqrt(30.0 / (c->agc_acc / 32 + 1e-12));
        c->half ^= 1;
        return;
    }
    gr = yr * c->gain; gi = yi * c->gain;
    for (k = VC_EQN - 1; k > 0; k--) { c->xr[k] = c->xr[k - 1]; c->xi[k] = c->xi[k - 1]; }
    c->xr[0] = gr; c->xi[0] = gi;
    c->half ^= 1;
    if (c->half) return;
    rx_timing(c, p);
    if (c->rxmode == 1) {
        double pp = gr * c->o2r + gi * c->o2i;
        c->o2r = c->o1r; c->o2i = c->o1i; c->o1r = gr; c->o1i = gi;
        c->rsym++;
        if (c->rsym > 4 && pp < -5.0) c->sbar_neg++; else c->sbar_neg = 0;
        if (c->sbar_neg < 2) return;
        c->rxmode = 2; c->rsym = 0;
        c->ref_idx = 1 - VC_EQN / 4;
        if (c->n_train < 4) c->t_sbar[c->n_train] = c->nsamp;
    }
    rx_symbol(c);
}

static void rx_tick(v32call_t *c, double x)
{
    double t;
    c->ring[c->rpos] = x; c->rpos = (c->rpos + 1) & 63;
    /* band-edge resonators at 600 and 3000 Hz */
    t = (c->lr * cos(2 * PI * 0.075) - c->li * sin(2 * PI * 0.075)) * (1 - 1.0 / 64) + x;
    c->li = (c->li * cos(2 * PI * 0.075) + c->lr * sin(2 * PI * 0.075)) * (1 - 1.0 / 64); c->lr = t;
    t = (c->ur * cos(2 * PI * 0.375) - c->ui * sin(2 * PI * 0.375)) * (1 - 1.0 / 64) + x;
    c->ui = (c->ui * cos(2 * PI * 0.375) + c->ur * sin(2 * PI * 0.375)) * (1 - 1.0 / 64); c->ur = t;
    c->tnext -= 1;
    if (c->tnext <= 0) {
        double p = -c->tnext, ar = 0, ai = 0, tt, cs, sn;
        int ph = (int)(p * VC_RXP), i, idx = c->rpos - 1;
        if (ph >= VC_RXP) ph = VC_RXP - 1;
        if (ph < 0) ph = 0;
        for (i = 0; i < VC_RXN; i++) {
            double v = c->ring[(idx - i) & 63];
            ar += v * c->bank[ph][i][0]; ai += v * c->bank[ph][i][1];
        }
        tt = (double)(c->nsamp % 40) - p;
        cs = cos(WC * tt); sn = sin(WC * tt);
        c->tnext += SPS / 2;
        rx_half(c, ar * cs + ai * sn, ai * cs - ar * sn, p);
        if (c->tnext <= 0) c->tnext = 1e-3;
    }
}

/* ======================= echo canceller ======================= */
static double ec_tick(v32call_t *c, double ref, double rx)
{
    double est = 0, en = 1e-6, err;
    int i, m = VC_EC_HIST - 1;
    c->xpos = (c->xpos + 1) & m;
    c->xh[c->xpos] = ref;
    for (i = 0; i < VC_EC_NEAR; i++) { double x = c->xh[(c->xpos - i) & m]; est += c->hn[i] * x; en += x * x; }
    for (i = 0; i < VC_EC_FAR; i++)  { double x = c->xh[(c->xpos - c->bulk - i) & m]; est += c->hf[i] * x; en += x * x; }
    err = rx - est;
    c->ec_in += (rx * rx - c->ec_in) / 512; c->ec_out += (err * err - c->ec_out) / 512;
    if (c->mu > 0 && en > 1.0) {
        double g = c->mu * err / en;
        for (i = 0; i < VC_EC_NEAR; i++) c->hn[i] += g * c->xh[(c->xpos - i) & m];
        for (i = 0; i < VC_EC_FAR; i++)  c->hf[i] += g * c->xh[(c->xpos - c->bulk - i) & m];
    }
    return err;
}

/* ======================= the calling modem ======================= */
static unsigned rate_word(unsigned rates)
{
    unsigned w = 0x0991u;                   /* 0000 1--1 1--1 -001 */
    if (rates & 1)  w |= 1u << 10;          /* B5: 4800 */
    if (rates & 2)  w |= 1u << 6;           /* B9: 7200 */
    if (rates & 4)  w |= 1u << 9;           /* B6: 9600 */
    if (rates & 8)  w |= 1u << 5;           /* B10: 12000 */
    if (rates & 16) w |= 1u << 3;           /* B12: 14400 */
    return w;
}

static unsigned word_rates(unsigned w)
{
    return ((w >> 10) & 1) | ((w >> 6) & 1) << 1 | ((w >> 9) & 1) << 2 | ((w >> 5) & 1) << 3 | ((w >> 3) & 1) << 4;
}

static void go(v32call_t *c, vc_state s) { c->state = s; c->seg = 0; c->t_state = 0; }

static void start(v32call_t *c)
{
    go(c, VCS_AA);
    vc_tx_set(&c->tx, VC_AA);
    c->rxmode = 0;
    c->rev_count = 0; c->ac_run = 0; c->s_run = 0;
    c->n_train = 0; c->mu = 0;
    c->tx_ready = c->rx_ready = 0;
    c->r1 = c->r3 = 0;
}

void v32call_init(v32call_t *c, const vc_cfg *cfg)
{
    int p, i;
    double norm = 0, D = (VC_RXN - 1) / 2.0;
    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    if (c->cfg.trn_len < 1280) c->cfg.trn_len = 1280;
    vc_tx_init(&c->tx, 0, cfg->level, cfg->getbit, cfg->user);
    for (i = 0; i < VC_RXN; i++) norm += rrc((i - D) / SPS, VC_ALPHA) * (0.5 + 0.5 * cos(PI * (i - D) / (D + 1)));
    for (p = 0; p < VC_RXP; p++)
        for (i = 0; i < VC_RXN; i++) {
            double t = i - D - (double)p / VC_RXP;
            double g = 2 * rrc(t / SPS, VC_ALPHA) * (0.5 + 0.5 * cos(PI * t / (D + 1))) / norm;
            c->bank[p][i][0] = g * cos(WC * t); c->bank[p][i][1] = g * sin(WC * t);
        }
    c->tnext = SPS / 2;
    c->bulk = VC_EC_NEAR;
    c->rate = -1;
    start(c);
}

int v32call_retrain(v32call_t *c)
{
    if (c->state != VCS_DATA) return -1;
    c->retrains++;
    start(c);
    return 0;
}

/* once per transmit symbol, before it is generated */
static void tx_control(v32call_t *c)
{
    vc_tx *t = &c->tx;
    c->seg++;
    switch (c->state) {
    case VCS_AA64:
        /* 6.1: the AA-CC transition leaves 64 symbol periods after the reversal arrived */
        if ((double)c->nsamp + VC_TX_DELAY >= (double)c->t_rev1 + 64 * SPS - SPS / 2) {
            vc_tx_set(t, VC_CC);
            c->t_cc = c->nsamp + (long)lrint(VC_TX_DELAY);
            go(c, VCS_CC);
        }
        break;
    case VCS_S_NT:
        /* S for NT, then the 256 symbols of S proper: an even count keeps ABAB going */
        if (c->seg > (long)(c->nt / SPS / 2) * 2) { go(c, VCS_S); c->seg = 1; }
        break;
    case VCS_S:
        if (c->seg > 256) { vc_tx_set(t, VC_SBAR); go(c, VCS_SBAR); c->seg = 1; }
        break;
    case VCS_SBAR:
        if (c->seg > 16) {
            int d = (int)(c->nt - 64 * SPS) - 24;
            vc_tx_set(t, VC_TRN); go(c, VCS_TRN); c->seg = 1;
            c->bulk = d < VC_EC_NEAR ? VC_EC_NEAR : d > VC_EC_HIST - VC_EC_FAR - 1 ? VC_EC_HIST - VC_EC_FAR - 1 : d;
            memset(c->hn, 0, sizeof c->hn); memset(c->hf, 0, sizeof c->hf);
            c->mu = 1.0;
            /* the far echo of TRN is back only after the round trip: train for that much longer */
            c->trn_run = c->cfg.trn_len + (long)((c->nt - 64 * SPS) / SPS);
            if (c->trn_run > 8192) c->trn_run = 8192;
        }
        break;
    case VCS_TRN:
        if (c->seg == c->trn_run - 512) c->mu = 0.25;
        if (c->seg == c->trn_run - 192) c->mu = 0.0625;
        if (c->seg > c->trn_run) {
            c->erle_db = 10 * log10((c->ec_in + 1e-9) / (c->ec_out + 1e-9));
            c->mu = 0;
            t->rword = rate_word(c->cfg.rates & word_rates(c->r1));
            vc_tx_set(t, VC_RATE); go(c, VCS_R2); c->seg = 1;
            c->s_run = 0;
        }
        break;
    case VCS_R2:
        if (c->r3 && t->rbit == 0) {
            unsigned common = word_rates(c->r3) & word_rates(c->r1) & c->cfg.rates;
            int k;
            c->rate = -1;
            for (k = 0; k < 5; k++) if (common & (1u << k)) c->rate = k;
            if (c->rate < 0) { vc_tx_set(t, VC_SILENCE); go(c, VCS_FAILED); break; }
            t->rword = rate_word(1u << c->rate) | 0xF000u;
            go(c, VCS_E); c->seg = 1;
        }
        break;
    case VCS_E:
        if (c->seg > 8) { t->rate = c->rate; vc_tx_set(t, VC_ONES); go(c, VCS_B1); c->seg = 1; }
        break;
    case VCS_B1:
        /* scrambled ones until the answerer's E has come and 128 T have passed */
        if (c->rxmode == 4 && c->clamp == 0) {
            vc_tx_set(t, VC_DATA); go(c, VCS_DATA);
            c->tx_ready = c->rx_ready = 1; c->connected = 1; c->bitrate = 2400 * (2 + c->rate);
        }
        break;
    default: break;
    }
}

static void rx_control(v32call_t *c)
{
    switch (c->state) {
    case VCS_AA:
        if (c->rev_count >= 1) {
            c->t_rev1 = c->rev_t - (long)lrint(VC_REV_LAT);
            go(c, VCS_AA64);
        }
        break;
    case VCS_CC:
        if (c->rev_count >= 2) {
            c->t_rev2 = c->rev_t - (long)lrint(VC_REV_LAT);
            c->nt = c->t_rev2 - c->t_cc;
            vc_tx_set(&c->tx, VC_SILENCE);
            go(c, VCS_LISTEN);
            c->s_run = 0;
        }
        break;
    case VCS_LISTEN:
    case VCS_R2:
        if (c->rxmode == 0 && c->s_run >= 40 && c->t_state > 200) {
            c->rxmode = 1; rx_reset_train(c);
            if (c->n_train < 4) c->t_s[c->n_train] = c->nsamp;
        }
        if (c->rxmode == 3 && c->rate_seen >= 2 && ((c->state == VCS_LISTEN && !c->r1) || (c->state == VCS_R2 && !c->r3))) {
            if (c->n_train < 4) { c->t_rate[c->n_train] = c->nsamp; c->mse_at_rate[c->n_train] = c->mse; }
            c->n_train++;
            if (c->state == VCS_LISTEN) {
                c->r1 = c->rate_word;
                c->rxmode = 0; c->s_run = 0;
                vc_tx_set(&c->tx, VC_S);
                go(c, VCS_S_NT);
            } else c->r3 = c->rate_word;
        }
        if (c->t_state > 8000L * 12) go(c, VCS_FAILED);
        break;
    case VCS_DATA:
        /* 7.1: the answerer's AC for more than 128 T asks for a retrain */
        if (c->ac_run > 300) { c->retrains++; start(c); }
        break;
    default: break;
    }
}

double v32call_tick(v32call_t *c, double rx)
{
    double tx, e;
    c->nsamp++; c->t_state++;
    if (vc_tx_symbol_due(&c->tx)) tx_control(c);
    tx = vc_tx_tick(&c->tx);
    e = ec_tick(c, c->tx.ref, rx);
    rx_tick(c, e);
    rx_control(c);
    return tx;
}
