/* What modem/answer.c is to do for a V.34 caller, in small: modem/v8ans,
 * then modem/v34ans when V.8 selected V.34 without PCM. Shared by
 * tools/rec_v34.c and rig/bench_v34.c so that both run the same thing. */
#ifndef V90D_RIG_V34GLUE_H
#define V90D_RIG_V34GLUE_H
#include <string.h>
#include "../modem/v8ans.h"
#include "../modem/v8msg.h"
#include "../modem/v34ans.h"

typedef struct { v8ans_t v8; v34ans_t m; v34ans_cfg cfg; int phase; } v34glue_t;

static void v34glue_init(v34glue_t *a, const v34ans_cfg *cfg)
{
    v8_menu_t local;
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    memset(&local, 0, sizeof local);
    local.call_function = V8_CALLF_DATA;
    local.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21;
    local.n_mod_octets = 3;
    local.has_pcm = 1; local.pcm = V8_PCM_DIGITAL;
    v8ans_init(&a->v8, 8000, (int16_t)(cfg->level * 181 / 128), &local);
}

static int16_t v34glue_tick(v34glue_t *a, int16_t rx)
{
    int16_t tx = 0;
    if (a->phase == 0) {
        if (v8ans_tick(&a->v8, rx, &tx) == V8A_DONE) {
            if (a->v8.result == V8A_RES_OK && a->v8.selected == 1) { v34ans_init(&a->m, &a->cfg); a->phase = 1; }
            else a->phase = 2;
        }
        return tx;
    }
    return a->phase == 1 ? v34ans_tick(&a->m, rx) : 0;
}

typedef struct { uint32_t s; long n, bad; } v34prbs;
static int v34prbs_next(v34prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
#endif
