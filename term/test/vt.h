/* A small ANSI terminal for the tests: plays what the menu sends into a
 * grid of cells, so that a test can check the caller sees what the menu
 * meant (characters and colours), diffs and all. Test code only. */
#ifndef V90D_TERM_VT_H
#define V90D_TERM_VT_H
#include <string.h>
#include "tscr.h"

typedef struct { tscr_cell c[TSCR_COLS * TSCR_ROWS]; int x, y, fg, bg, bold, st, p[8], np, bad; } vt_t;

static const int VT_CGA[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };   /* ANSI order to CGA (its own inverse) */

static void vt_init(vt_t *v)
{
    int i;
    memset(v, 0, sizeof *v);
    for (i = 0; i < TSCR_COLS * TSCR_ROWS; i++) { v->c[i].ch = ' '; v->c[i].fg = TS_W; v->c[i].bg = TS_K; }
    v->fg = 7;
}

static void vt_byte(vt_t *v, unsigned char ch)
{
    int i;
    if (v->st == 1) { v->st = ch == '[' ? 2 : 0; v->np = 0; v->p[0] = 0; if (ch != '[') v->bad++; return; }
    if (v->st == 2) {
        if (ch >= '0' && ch <= '9') { v->p[v->np] = v->p[v->np] * 10 + (ch - '0'); return; }
        if (ch == ';') { if (v->np < 7) v->p[++v->np] = 0; return; }
        v->np++;
        v->st = 0;
        if (ch == 'H') { v->y = v->p[0] - 1; v->x = (v->np > 1 ? v->p[1] : 1) - 1; }
        else if (ch == 'J') { for (i = 0; i < TSCR_COLS * TSCR_ROWS; i++) { v->c[i].ch = ' '; v->c[i].fg = TS_W; v->c[i].bg = (uint8_t)v->bg; } }
        else if (ch == 'm') {
            for (i = 0; i < v->np; i++) {
                int a = v->p[i];
                if (a == 0) { v->fg = 7; v->bg = 0; v->bold = 0; }
                else if (a == 1) v->bold = 1;
                else if (a >= 30 && a <= 37) v->fg = VT_CGA[a - 30];
                else if (a >= 40 && a <= 47) v->bg = VT_CGA[a - 40];
                else v->bad++;
            }
        } else v->bad++;
        return;
    }
    if (ch == 0x1b) { v->st = 1; return; }
    if (ch == '\r') { v->x = 0; return; }
    if (ch == '\n') { if (v->y < TSCR_ROWS - 1) v->y++; return; }
    if (ch < 32) { v->bad++; return; }
    if (v->x < TSCR_COLS && v->y < TSCR_ROWS) {
        tscr_cell *c = &v->c[v->y * TSCR_COLS + v->x];
        c->ch = ch; c->fg = (uint8_t)(v->fg + (v->bold ? 8 : 0)); c->bg = (uint8_t)v->bg;
    }
    v->x++;
}

static void vt_play(vt_t *v, const uint8_t *p, size_t n) { size_t i; for (i = 0; i < n; i++) vt_byte(v, p[i]); }

/* cells that differ from the screen (a blank's foreground does not matter;
 * the bottom right cell is never sent) */
static int vt_diff(const vt_t *v, const tscr *s)
{
    int i, n = 0;
    for (i = 0; i < TSCR_COLS * TSCR_ROWS - 1; i++) {
        const tscr_cell *a = &v->c[i], *b = &s->c[i];
        if (a->ch != b->ch || (a->bg & 7) != (b->bg & 7) || (b->ch != ' ' && a->fg != b->fg)) n++;
    }
    return n;
}
#endif
