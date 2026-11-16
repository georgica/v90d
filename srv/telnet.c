/* See telnet.h. */
#include <string.h>
#include "telnet.h"

enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
enum { O_BINARY = 0, O_ECHO = 1, O_SGA = 3, O_TTYPE = 24, O_NAWS = 31 };
enum { T_DATA = 0, T_IAC, T_OPT, T_SB_OPT, T_SB, T_SB_IAC };

void telnet_init(telnet_t *t) { memset(t, 0, sizeof *t); }

static void put3(uint8_t *rep, size_t *rn, uint8_t cmd, uint8_t opt)
{
    rep[(*rn)++] = IAC; rep[(*rn)++] = cmd; rep[(*rn)++] = opt;
}
/* WILL / WONT / DO / DONT opt from the board */
static void option(telnet_t *t, uint8_t cmd, uint8_t opt, uint8_t *rep, size_t *rn)
{
    int *him = opt == O_BINARY ? &t->him_bin : opt == O_ECHO ? &t->him_echo : opt == O_SGA ? &t->him_sga : NULL;
    int *us = opt == O_BINARY ? &t->us_bin : opt == O_SGA ? &t->us_sga : opt == O_TTYPE ? &t->us_ttype :
              opt == O_NAWS ? &t->us_naws : NULL;
    switch (cmd) {
    case WILL:
        if (!him) put3(rep, rn, DONT, opt);
        else if (!*him) { *him = 1; put3(rep, rn, DO, opt); if (opt == O_BINARY) t->cr_rx = 0; }
        break;
    case WONT:
        if (him && *him) { *him = 0; put3(rep, rn, DONT, opt); }
        break;
    case DO:
        if (!us) put3(rep, rn, WONT, opt);
        else if (!*us) {
            *us = 1; put3(rep, rn, WILL, opt);
            if (opt == O_BINARY) t->cr_tx = 0;
            if (opt == O_NAWS) {
                static const uint8_t w[9] = { IAC, SB, O_NAWS, 0, 80, 0, 24, IAC, SE };
                memcpy(rep + *rn, w, sizeof w); *rn += sizeof w;
            }
        }
        break;
    default:    /* DONT */
        if (us && *us) { *us = 0; put3(rep, rn, WONT, opt); }
        break;
    }
}

size_t telnet_rx(telnet_t *t, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_n,
                 uint8_t *rep, size_t rep_cap, size_t *rep_n)
{
    size_t i;
    *out_n = 0; *rep_n = 0;
    for (i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (rep_cap - *rep_n < TELNET_REPLY_MAX || rep_cap < TELNET_REPLY_MAX) break;
        switch (t->st) {
        case T_DATA:
            if (c == IAC) { t->st = T_IAC; break; }
            if (t->cr_rx && c == 0) { t->cr_rx = 0; break; }           /* CR NUL: the CR has gone out */
            if (*out_n == out_cap) return i;
            out[(*out_n)++] = c;
            t->cr_rx = c == '\r' && !t->him_bin;
            break;
        case T_IAC:
            if (c == IAC) {
                if (*out_n == out_cap) return i;
                out[(*out_n)++] = IAC; t->cr_rx = 0; t->st = T_DATA;
            } else if (c == WILL || c == WONT || c == DO || c == DONT) { t->cmd = c; t->st = T_OPT; }
            else if (c == SB) t->st = T_SB_OPT;
            else t->st = T_DATA;                                       /* NOP, GA and the rest: nothing to do */
            break;
        case T_OPT:
            option(t, t->cmd, c, rep, rep_n);
            t->st = T_DATA;
            break;
        case T_SB_OPT:
            t->sb_opt = c; t->sb_n = 0; t->sb_first = 0; t->st = T_SB;
            break;
        case T_SB:
            if (c == IAC) { t->st = T_SB_IAC; break; }
            if (t->sb_n++ == 0) t->sb_first = c;
            break;
        default:    /* T_SB_IAC */
            if (c == IAC) { if (t->sb_n++ == 0) t->sb_first = IAC; t->st = T_SB; break; }
            if (c == SE && t->sb_opt == O_TTYPE && t->sb_n == 1 && t->sb_first == 1 && t->us_ttype) {
                static const uint8_t ty[10] = { IAC, SB, O_TTYPE, 0, 'A', 'N', 'S', 'I', IAC, SE };
                memcpy(rep + *rep_n, ty, sizeof ty); *rep_n += sizeof ty;
            }
            t->st = T_DATA;                                            /* SE, or something that ends it anyway */
            break;
        }
    }
    return i;
}

size_t telnet_tx(telnet_t *t, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_n)
{
    size_t i;
    *out_n = 0;
    for (i = 0; i < n; i++) {
        uint8_t c = in[i];
        int nul = t->cr_tx && c != '\n';                               /* a CR that is not CR LF is CR NUL */
        if (out_cap - *out_n < (size_t)nul + (c == IAC ? 2u : 1u)) break;
        if (nul) out[(*out_n)++] = 0;
        out[(*out_n)++] = c;
        if (c == IAC) out[(*out_n)++] = IAC;
        t->cr_tx = c == '\r' && !t->us_bin;
    }
    return i;
}
