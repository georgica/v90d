/* Test peer: V.90 Phases 3 / 4, analogue side. See p34a.h. */
#include <string.h>
#include <math.h>
#include "p34a.h"
#include "../v90/v90_pcm.h"

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }
static int  getbitm(const uint8_t *m, int u) { return (m[u >> 3] >> (u & 7)) & 1; }

static void cons(uint8_t *mask, int M, int top, int step)
{
    int k;
    memset(mask, 0, 16);
    for (k = 0; k < M; k++) setbit(mask, top - k * step);
}

/* CP: D = drn + 20 = Kd + 6 - Sr */
static void build_cp(p34a_t *p, int Kd)
{
    const p34a_cfg *cfg = &p->cfg;
    int i;
    memset(&p->cp, 0, sizeof p->cp);
    p->cp.is_cp = 1; p->cp.drn = (uint8_t)(Kd + 6 - cfg->sr_d - 20);
    p->cp.sr = (uint8_t)cfg->sr_d; p->cp.ld = (uint8_t)cfg->ld_d; p->cp.alaw = (uint8_t)cfg->alaw;
    p->cp.rates = cfg->up_rates; p->cp.rms_ratio = 0x2000;
    if (cfg->sr_d) { p->cp.a1 = 60; p->cp.a2 = -58; p->cp.b1 = 10; p->cp.b2 = -10; }
    for (i = 0; i < 6; i++) p->cp.idx[i] = (uint8_t)(i % 3);
    cons(p->cp.mask[0], cfg->Md, 126, 1);
    cons(p->cp.mask[1], cfg->Md, 125, 1);
    cons(p->cp.mask[2], cfg->Md, 127, 1);
}

/* ------------------------------------------------------------------ */
/* upstream                                                            */

static void unext(p34a_t *p, p34a_ustate s) { p->us = s; p->un = 0; }

static void load_frame(p34a_t *p, int which)   /* 0 Ja, 1 CPt, 2 CP, 3 CP' */
{
    int n;
    if (which == 0) n = v90_dil_build(&p->dil, p->fr);
    else if (which == 1) n = v90_cp_build(&p->cpt, p->fr);
    else { p->cp.ack = (uint8_t)(which == 3); n = v90_cp_build(&p->cp, p->fr); }
    p->frn = n > 0 ? n : 0; p->frpos = 0;
    p->fr_ack = which == 3;
    if (p->frn & 1) p->odd_frames++;
}

static int pick_up_rate(const p34a_t *p)
{
    unsigned common = (unsigned)p->mp.rates & (unsigned)p->cfg.up_rates;
    int k;
    for (k = 12; k >= 0; k--)
        if ((common & (1u << k)) && 4800 + 2400 * k <= p->mp.max_rate * 2400) return 4800 + 2400 * k;
    return 0;
}

static void start_data(p34a_t *p)
{
    v34_cfg_t c;
    memset(&c, 0, sizeof c);
    p->up_rate = pick_up_rate(p);
    c.symbol_rate = (uint16_t)p->cfg.sym_rate; c.data_rate = (uint16_t)p->up_rate;
    c.trellis = p->mp.trellis; c.expanded = p->mp.shaping; c.nonlinear = p->mp.nonlinear;
    c.scrambler = V34_GPA;
    memcpy(c.h, p->mp.h, sizeof c.h);
    if (!p->up_rate || v34_params_init(&p->vpar, &c) != 0) { p->up_rate = 0; unext(p, U_SIL1); return; }
    v34_tx_init(&p->vtx, &p->vpar, p->cfg.getbit, p->cfg.user);
    p->data_starts++;
    p->vscale = v34_gain_q14(p->vpar.energy_tx_q8) / 16384.0 / V34_ONE;
    unext(p, U_DATA);
}

/* the next bit of the CPt / CP stream, with the transitions of 9.4.2 made at
 * sequence boundaries */
static int seq_bit(p34a_t *p)
{
    if (p->us == U_SCR) {
        if (++p->un > 2 * p->cfg.scr_syms) { unext(p, U_CP); load_frame(p, 2); }
        else return 1;
    }
    if (p->us == U_E) {
        if (p->e_left > 0) { p->e_left--; return 1; }
        return 0;                                   /* (a pad bit when E ended inside a symbol) */
    }
    if (p->frpos >= p->frn) {
        if (p->us == U_CPT) {
            if (p->ribar_seen) {
                if (p->cfg.scr_syms > 0) { unext(p, U_SCR); return seq_bit(p); }
                unext(p, U_CP); load_frame(p, 2);
            } else {
                load_frame(p, 1);
            }
        } else if (p->cp.silence) {
            /* 9.6.2.1.5, .6: CPs until MP, CPs' until Ed, then SCR and CP proper */
            if (p->ed_seen) {
                p->cp.silence = 0;
                p->ed_seen = p->mp_seen = p->mpack_seen = 0; p->cp_ack_sent = 0;
                p->cfg.scr_syms = 200;
                unext(p, U_SCR);
                return seq_bit(p);
            }
            load_frame(p, p->mp_seen ? 3 : 2);
        } else {    /* U_CP */
            if (p->fr_ack) p->cp_ack_sent++;
            if (p->cp_ack_sent >= 1 && (p->mpack_seen || p->ed_seen)) {
                unext(p, U_E); p->e_left = 20;
                return seq_bit(p);
            }
            load_frame(p, (p->mp_seen || p->mpack_seen) ? 3 : 2);
        }
    }
    return p->fr[p->frpos++];
}

static void up_sym(void *u, double *re, double *im)
{
    p34a_t *p = (p34a_t *)u;
    v34_pt_t pt; double s = 1.0 / V34_ONE, r, i;
    int n = p->un;
    pt.re = pt.im = 0;

    switch (p->us) {
    case U_SIL0:
        if (++p->un >= p->cfg.sil0) unext(p, U_S1);
        break;
    case U_S1: case U_S3:
        pt = v34_s_point((unsigned)n, 128);
        if (++p->un >= 128) unext(p, p->us == U_S1 ? U_SBAR1 : U_SBAR3);
        break;
    case U_SBAR1: case U_SBAR2: case U_SBAR3:
        pt = v34_sbar_point((unsigned)n);
        if (++p->un >= 16) {
            if (p->us == U_SBAR1) unext(p, U_PP);
            else if (p->us == U_SBAR2 && p->cfg.dil_n > 0) unext(p, U_DILWAIT);
            else {
                /* 8.5.2: scrambler and differential encoder zero before the first CPt */
                v34_tmod_init(&p->tmod, V34_GPA, 4);
                unext(p, U_CPT); load_frame(p, 1);
            }
        }
        break;
    case U_PP: {
        int16_t a, b;
        v34_pp_point((unsigned)n, &a, &b);
        /* unit magnitude, sent at the power of the 4-point signals */
        *re = a / 16384.0 * sqrt(2.0); *im = b / 16384.0 * sqrt(2.0);
        if (++p->un >= V34_PP_LEN) { unext(p, U_TRN); v34_tmod_init(&p->tmod, V34_GPA, 4); }
        return;
    }
    case U_TRN:
        pt = v34_tmod_trn(&p->tmod);
        if (++p->un >= p->cfg.ntrn) { unext(p, U_JA); load_frame(p, 0); }
        break;
    case U_JA:
        if (p->sdbar_seen) { unext(p, U_SIL1); p->rot += p->cfg.rot2; break; }   /* 9.3.2.4 */
        {
            unsigned b0, b1;
            if (p->frpos >= p->frn) p->frpos = 0;
            b0 = p->fr[p->frpos++];
            if (p->frpos >= p->frn) p->frpos = 0;
            b1 = p->fr[p->frpos++];
            pt = v34_tmod_put(&p->tmod, b0 | (b1 << 1));
        }
        p->un++;
        break;
    case U_SIL1:
        if (p->jd_ok && ++p->un > p->cfg.jd_wait) unext(p, U_S2);
        break;
    case U_S2:
        /* open-ended S that must end on the rotated point */
        pt = v34_s_point((unsigned)(n & 1), 2);
        p->un++;
        if (p->jdp_seen && p->un >= 64 && (p->un & 1) == 0) unext(p, U_SBAR2);
        break;
    case U_DILWAIT:
        if (p->cfg.scr_in_dil) pt = v34_tmod_put(&p->tmod, 3);
        if (++p->un >= p->cfg.dil_syms) { unext(p, U_S3); if (!p->cfg.scr_in_dil) p->rot += p->cfg.rot2; }
        break;
    case U_CPT: case U_SCR: case U_CP: case U_E: {
        unsigned b0 = (unsigned)seq_bit(p), b1;
        if (p->us == U_E && p->e_left == 0 && b0 == 1) {
            /* that was the 20th one, in the first half of a symbol */
            b1 = 0;
        } else {
            b1 = (unsigned)seq_bit(p);
        }
        pt = v34_tmod_put(&p->tmod, b0 | (b1 << 1));
        if (p->us == U_E && p->e_left == 0) start_data(p);
        break;
    }
    case U_RS:
        pt = v34_s_point((unsigned)n, 128);
        if (++p->un >= 128) unext(p, U_RSBAR);
        break;
    case U_RSBAR:
        pt = v34_sbar_point((unsigned)n);
        if (++p->un >= 16) {
            /* 8.5.2 resets the scrambler before the first CPt only: here it
             * just goes on from wherever the earlier sequences left it */
            p->cp_ack_sent = 0;
            if (p->reneg_scr > 0) { p->cfg.scr_syms = p->reneg_scr; unext(p, U_SCR); }
            else { unext(p, U_CP); load_frame(p, 2); }
        }
        break;
    case U_DATA: {
        v34_pt_t x;
        v34_tx_symbol(&p->vtx, &x, 0);
        p->tx_ready = !v34_tx_in_b1(&p->vtx);
        r = x.re * p->vscale; i = x.im * p->vscale;
        *re = r * cos(p->rot) - i * sin(p->rot); *im = i * cos(p->rot) + r * sin(p->rot);
        return;
    }
    }
    r = pt.re * s; i = pt.im * s;
    *re = r * cos(p->rot) - i * sin(p->rot); *im = i * cos(p->rot) + r * sin(p->rot);
}

/* ------------------------------------------------------------------ */
/* downstream: octets as sent                                          */

static void dnext(p34a_t *p, p34a_dstate s) { p->ds = s; p->dn = 0; }

static int is_unit(const p34a_t *p, const uint8_t *o, unsigned u0, unsigned u1, int inv)
{
    int i;
    for (i = 0; i < 6; i++)
        if (o[i] != v90_pcm_octet(p->cfg.alaw, (i % 3 == 1) ? u1 : u0, ((i < 3) ? 1 : 0) ^ inv)) return 0;
    return 1;
}

/* +1: the six octets are an Rd unit (+ + + - - -), -1: the inverse, 0: neither */
static int r_unit_map(const p34a_t *p, const v90_map_t *m, const uint8_t *o)
{
    int i, pos = 1, neg = 1;
    for (i = 0; i < 6; i++) {
        unsigned u = m->C[i][0];
        if (o[i] != v90_pcm_octet(p->cfg.alaw, u, i < 3)) pos = 0;
        if (o[i] != v90_pcm_octet(p->cfg.alaw, u, i >= 3)) neg = 0;
    }
    return pos ? 1 : neg ? -1 : 0;
}
static int r_unit(const p34a_t *p, const uint8_t *o) { return r_unit_map(p, &p->dmap, o); }

/* bits of TRN2d / MP / Ed, descrambled */
static void train_bit(p34a_t *p, int bit)
{
    if (p->mbn > 0 && p->mbn < (int)sizeof p->mb) p->mb[p->mbn++] = (uint8_t)bit;
    if (bit) { p->mones++; return; }
    if (p->mones >= 17 && p->mbn == 0) {
        memset(p->mb, 1, 17); p->mb[17] = 0; p->mbn = 18;
    }
    p->mones = 0;
}

static void train_frame_done(p34a_t *p, const uint8_t *bits, int D)
{
    int k, zeros = 0, ones = 0;
    for (k = 0; k < D; k++) { if (bits[k]) ones++; else zeros++; }
    if (!p->mp_seen && !p->mpack_seen && p->mbn == 0) {
        p->trn2d_frames++;
        if (zeros && p->mones < 17) p->trn2d_bad++;
    }
    if (p->mbn >= V90_MP0_BITS) {
        v90_mp_t got;
        int r = v90_mp_parse(p->mb, p->mbn, &got);
        if (r > 0) {
            p->mp = got; p->mp_n++;
            if (got.max_rate == 0) p->clear_seen = 1;
            if (got.ack) p->mpack_seen = 1; else p->mp_seen = 1;
            p->mbn = 0; p->mones = 0;
        } else if (p->mbn >= V90_MP1_BITS + 48) {
            p->mbn = 0;
        }
    }
    /* Ed: two data frames of zeros */
    if (zeros == D) { if (++p->zero_frames == 2) p->ed_seen = 1; }
    else p->zero_frames = 0;
}

static void down_octet(p34a_t *p, uint8_t o, const uint8_t *ahead)
{
    int alaw = p->cfg.alaw;
    unsigned W = (unsigned)p->cfg.uinfo + 16, U = (unsigned)p->cfg.uinfo;
    int sign = v90_octet_sign(o);
    unsigned uc = v90_octet_ucode(alaw, o);

    switch (p->ds) {
    case D_WAIT_SD:
        /* `ahead` holds this octet and the next 11: Sd unit then Sd-bar unit */
        if (is_unit(p, ahead, W, 0, 0) && is_unit(p, ahead + 6, W, 0, 1)) {
            /* this octet starts the last Sd unit */
            p->sd_at = p->tick;
            dnext(p, D_SDBAR); p->dn = -5;
        }
        break;

    case D_SDBAR:
        if (p->dn == 5) p->sdbar_seen = 1;                  /* one Sd-bar unit in */
        if (++p->dn >= 48) {
            v90dec_sign_init(&p->dt); v90dec_sign_init(&p->dj);
            p->sbn = 0; p->sones = 0;
            dnext(p, D_SIGN);
        }
        break;

    case D_SIGN: {
        /* TRN1d, Jd, Jd': UINFO with a sign */
        int bt = v90dec_trn1d(&p->dt, sign), bj = v90dec_jd(&p->dj, sign);
        if (uc != U) p->sign_bad++;
        if (!p->jd_ok && p->sbn == 0 && p->dn < 2040) {
            p->trn1d_n++;
            if (p->dn >= 23 && bt != 1) p->trn1d_bad++;
        }
        p->dn++;
        if (p->sbn > 0 && p->sbn < (int)sizeof p->sb) p->sb[p->sbn++] = (uint8_t)bj;
        if (p->sbn == 0) {
            if (bj) p->sones++;
            else { if (p->sones >= 17) { memset(p->sb, 1, 17); p->sb[17] = 0; p->sbn = 18; } p->sones = 0; }
        } else if (p->sbn == V90_JD_BITS) {
            if (v90_jd_parse(p->sb, V90_JD_BITS, &p->jd) == V90_JD_BITS) p->jd_ok++;
            else { p->sbn = 0; p->sones = 0; }
        } else if (p->sbn == V90_JD_BITS + 12) {
            /* after a good Jd: another Jd (17 ones) or Jd' (12 zeros) */
            int k, z = 0;
            for (k = 0; k < 12; k++) if (!p->sb[V90_JD_BITS + k]) z++;
            if (z == 12) {
                p->jdp_seen = 1;
                p->seg = 0; p->segm = 0;
                dnext(p, p->dil.n > 0 ? D_DIL : D_RI);
            } else {
                memmove(p->sb, p->sb + V90_JD_BITS, 12); p->sbn = 12;
            }
        }
        break;
    }

    case D_DIL: {
        int u = p->dil.train[p->seg], c = u / 16, L = (p->dil.h[c] + 1) * 6;
        if (p->segm == 0 && is_unit(p, ahead, U, U, 0) && is_unit(p, ahead + 6, U, U, 0)) {
            dnext(p, D_RI);                                 /* the DIL ended on a segment boundary */
            down_octet(p, o, ahead);
            return;
        }
        {
            int sgn = getbitm(p->dil.sp, p->segm % p->dil.lsp);
            int mag = getbitm(p->dil.tp, p->segm % p->dil.ltp) ? u : p->dil.ref[c];
            if (o != v90_pcm_octet(alaw, (unsigned)mag, sgn)) p->dil_bad++;
        }
        if (++p->segm >= L) { p->segm = 0; p->seg = (p->seg + 1) % p->dil.n; p->dil_n_ok++; }
        break;
    }

    case D_RI:
        p->ri_seen = 1;
        if (p->dn % 6 == 0 && is_unit(p, ahead, U, U, 1)) {
            dnext(p, D_RIBAR);
            down_octet(p, o, ahead);
            return;
        }
        if (o != v90_pcm_octet(alaw, U, (p->dn % 6) < 3)) p->ri_bad++;
        p->dn++; p->ri_n++;
        break;

    case D_RIBAR:
        if (o != v90_pcm_octet(alaw, U, (p->dn % 6) >= 3)) p->ri_bad++;
        if (p->dn == 5) p->ribar_seen = 1;
        if (++p->dn >= 24) {
            v90dec_init(&p->dec, &p->tmap, alaw); v90_scr_init(&p->dscr);
            p->fon = 0; p->mbn = 0; p->mones = 0; p->zero_frames = 0;
            dnext(p, D_TRAIN);
        }
        break;

    case D_RD:
        /* 8.6.4: the largest point of each interval, + + + - - - or the
         * inverse (not differentially encoded); Rd-bar is the other one */
        if (p->dn % 6 == 0 && r_unit(p, ahead) == -p->rd_pol) {
            dnext(p, D_RDBAR);
            down_octet(p, o, ahead);
            return;
        }
        if (p->dn % 6 == 0) { if (r_unit(p, ahead) == p->rd_pol) p->rd_units++; else p->rd_bad++; }
        p->dn++;
        break;

    case D_RDBAR:
        if (p->dn % 6 == 0) { if (r_unit(p, ahead) == -p->rd_pol) p->rdbar_units++; else p->rd_bad++; }
        if (p->dn == 5 && p->us == U_DATA) {
            /* 9.6.2.2.2: the digital modem began it; answer with S */
            p->tx_ready = 0;
            unext(p, U_RS);
        }
        if (++p->dn >= 24) {
            v90dec_init(&p->dec, &p->tmap, alaw); v90_scr_init(&p->dscr);
            p->fon = 0; p->mbn = 0; p->mones = 0; p->zero_frames = 0;
            dnext(p, D_TRAIN);
        }
        break;

    case D_SIL:
        if (p->dn % 6 == 0 && r_unit_map(p, &p->tmap, ahead) != 0) {
            p->rd_pol = r_unit_map(p, &p->tmap, ahead);
            dnext(p, D_RT);
            down_octet(p, o, ahead);
            return;
        }
        p->sil_octets++;
        if (uc != 0) p->sil_bad++;
        p->dn++;
        break;

    case D_RT:
        if (p->dn % 6 == 0 && r_unit_map(p, &p->tmap, ahead) == -p->rd_pol) {
            dnext(p, D_RTBAR);
            down_octet(p, o, ahead);
            return;
        }
        if (p->dn % 6 == 0) { if (r_unit_map(p, &p->tmap, ahead) == p->rd_pol) p->rt_units++; else p->rd_bad++; }
        p->dn++;
        break;

    case D_RTBAR:
        if (p->dn % 6 == 0) { if (r_unit_map(p, &p->tmap, ahead) == -p->rd_pol) p->rtbar_units++; else p->rd_bad++; }
        if (++p->dn >= 24) {
            /* MP follows at once. Nothing says that the scrambler starts
             * anew here (8.6.5 says so for TRN2d only): carry on */
            p->fon = 0; p->mbn = 0; p->mones = 0; p->zero_frames = 0;
            dnext(p, D_TRAIN);
        }
        break;

    case D_TRAIN: case D_B1D: case D_DATA:
        if (p->ds == D_DATA && p->fon == 0 && r_unit(p, ahead) != 0) {
            p->rd_pol = r_unit(p, ahead);
            p->rx_ready = 0;
            p->mp_seen = p->mpack_seen = p->ed_seen = 0; p->b1d_frames = 0;
            dnext(p, D_RD);
            down_octet(p, o, ahead);
            return;
        }
        p->fo[p->fon++] = o;
        if (p->fon == 6) {
            uint8_t fb[V90_MAX_D], db[V90_MAX_D];
            const v90_map_t *m = p->ds == D_TRAIN ? &p->tmap : &p->dmap;
            int D = V90_MAP_D(m), k;
            p->fon = 0;
            if (v90dec_frame(&p->dec, p->fo, fb) != D) { p->dec_bad++; break; }
            for (k = 0; k < D; k++) db[k] = (uint8_t)v90_descramble_gpc(&p->dscr, fb[k]);
            if (p->ds == D_TRAIN) {
                for (k = 0; k < D; k++) train_bit(p, db[k]);
                train_frame_done(p, db, D);
                if (p->ed_seen && p->cp.silence) {
                    /* 9.6.1.2.5: silence, in data frame alignment; ed_seen is
                     * left for the upstream side to take */
                    dnext(p, D_SIL);
                } else if (p->ed_seen) {
                    (void)v90_map_from_cp(&p->dmap, &p->cp);    /* the CP that was acknowledged */
                    /* 8.6.1: everything from zero before B1d */
                    v90dec_init(&p->dec, &p->dmap, alaw); v90_scr_init(&p->dscr);
                    dnext(p, D_B1D);
                }
            } else if (p->ds == D_B1D) {
                for (k = 0; k < D; k++) if (db[k] != 1) p->b1d_bad++;
                if (++p->b1d_frames >= 48) { if (p->rd_units > 0) p->renegs++; p->rx_ready = 1; dnext(p, D_DATA); }
            } else if (p->cfg.putbit) {
                for (k = 0; k < D; k++) p->cfg.putbit(p->cfg.user, db[k]);
            }
        }
        break;
    }
}

/* ------------------------------------------------------------------ */

void p34a_init(p34a_t *p, const p34a_cfg *cfg)
{
    int i;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    qammod_init(&p->mod, cfg->sym_rate, cfg->ppm, cfg->fc, cfg->gain, up_sym, p);
    p->us = U_SIL0; p->ds = D_WAIT_SD;

    /* the DIL we ask for */
    p->dil.n = (uint8_t)cfg->dil_n; p->dil.lsp = 5; p->dil.ltp = 12;
    p->dil.sp[0] = 0x16;
    p->dil.tp[0] = 0xDB; p->dil.tp[1] = 0x06;
    for (i = 0; i < 8; i++) { p->dil.h[i] = (uint8_t)(i % 3); p->dil.ref[i] = (uint8_t)(16 * i + 3); }
    for (i = 0; i < p->dil.n; i++) p->dil.train[i] = (uint8_t)((i * 29 + 7) & 127);
    if (p->dil.n == 0) { p->dil.lsp = 1; p->dil.ltp = 1; }

    /* CPt: D = drn + 8 = Kt + 6 */
    p->cpt.is_cp = 0; p->cpt.drn = (uint8_t)(cfg->Kt + 6 - 8);
    p->cpt.alaw = (uint8_t)cfg->alaw; p->cpt.rates = cfg->up_rates; p->cpt.rms_ratio = 0x2000;
    for (i = 0; i < 6; i++) p->cpt.idx[i] = (uint8_t)(i & 1);
    cons(p->cpt.mask[0], cfg->Mt, 90, 9);
    cons(p->cpt.mask[1], cfg->Mt, 85, 8);
    (void)v90_map_from_cp(&p->tmap, &p->cpt);

    build_cp(p, cfg->Kd);
    (void)v90_map_from_cp(&p->dmap, &p->cp);
}

int p34a_renegotiate(p34a_t *p, int Kd, int cleardown)
{
    if (p->us != U_DATA || p->ds != D_DATA) return -1;
    if (Kd > 0) build_cp(p, Kd);
    if (cleardown) p->cp.drn = 0;
    p->cp.silence = (uint8_t)(p->reneg_silence != 0);
    p->tx_ready = 0;
    p->mp_seen = p->mpack_seen = p->ed_seen = 0;
    unext(p, U_RS);
    return 0;
}

double p34a_tick(p34a_t *p, uint8_t octet)
{
    /* a 12-octet delay line gives the downstream reader its look-ahead */
    uint8_t ahead[12];
    int i;
    p->tick++;
    memmove(p->look, p->look + 1, 11); p->look[11] = octet;
    if (++p->nlook >= 12) {
        for (i = 0; i < 12; i++) ahead[i] = p->look[i];
        down_octet(p, ahead[0], ahead);
    }
    return qammod_tick(&p->mod);
}
