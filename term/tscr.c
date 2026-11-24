/* See tscr.h. */
#include <string.h>
#include "tscr.h"

/* CP437 0x80..0xFF as Unicode */
static const uint16_t HIGH[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};

/* the same characters for a terminal without CP437 (plain mode) */
static const char PLAIN[129] =
    "CueaaaaceeeiiiAA" "EaAooouuyOUcLYPf" "aiounNao?--?" "?!<>"
    ".:#|++++++|+++++" "++++-++++++++=++" "+++++++++++#_###"
    "asGpSsutPOOd8pe^" "=+><()/~o.-vn2x ";

uint8_t tscr_cp437(uint32_t cp)
{
    int i;
    if (cp >= 0x20 && cp < 0x7F) return (uint8_t)cp;
    switch (cp) {                 /* stand-ins: not in CP437, or a control code there */
    case 0x00A0: return ' ';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return '-';
    case 0x2026: return '.';
    case 0x25BA: case 0x25B6: case 0x2192: return '>';
    case 0x25C4: case 0x25C0: case 0x2190: return '<';
    case 0x2191: case 0x25B2: return '^';
    case 0x2193: case 0x25BC: return 'v';
    case 0x25CF: case 0x2022: return '*';
    case 0x25A1: return 'o';
    }
    if (cp < 0x80) return '?';    /* control codes */
    for (i = 0; i < 127; i++) if (HIGH[i] == cp) return (uint8_t)(0x80 + i);   /* 0xFF (no-break space) is ' ' above */
    return '?';
}

/* one code point from UTF-8 at *p; a bad octet is one '?' */
static uint32_t next_cp(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t c = p[0];
    int n = c >= 0xF0 && c < 0xF5 ? 3 : c >= 0xE0 ? (c < 0xF0 ? 2 : -1) : c >= 0xC2 ? 1 : c >= 0x80 ? -1 : 0, i;
    if (n < 0) { *pp += 1; return '?'; }
    c &= n == 0 ? 0x7F : n == 1 ? 0x1F : n == 2 ? 0x0F : 0x07;
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pp += 1; return '?'; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *pp += n + 1;
    return c;
}

int tscr_len(const char *u)
{
    int n = 0;
    while (*u) { (void)next_cp(&u); n++; }
    return n;
}

void tscr_clear(tscr *s)
{
    int i;
    for (i = 0; i < TSCR_COLS * TSCR_ROWS; i++) { s->c[i].ch = ' '; s->c[i].fg = TS_W; s->c[i].bg = TS_K; }
    memset(s->px, TSCR_NONE, sizeof s->px);
    s->cx = -1; s->cy = 0;
}

int tscr_put(tscr *s, int x, int y, const char *u, int fg, int bg)
{
    int n = 0;
    while (*u) {
        uint8_t ch = tscr_cp437(next_cp(&u));
        if (x >= 0 && x < TSCR_COLS && y >= 0 && y < TSCR_ROWS) {
            tscr_cell *c = &s->c[y * TSCR_COLS + x];
            c->ch = ch; c->fg = (uint8_t)(fg & 15);
            if (bg >= 0) c->bg = (uint8_t)(bg & 15);
            n++;
        }
        x++;
    }
    return n;
}

void tscr_fill(tscr *s, int x, int y, int w, int h, const char *u, int fg, int bg)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) tscr_put(s, x + i, y + j, u, fg, bg);
}

void tscr_bar(tscr *s, int y, const char *left, const char *right, int fg, int bg)
{
    tscr_fill(s, 0, y, TSCR_COLS, 1, " ", fg, bg);
    tscr_put(s, 0, y, left, fg, bg);
    tscr_put(s, TSCR_COLS - tscr_len(right), y, right, fg, bg);
}

void tscr_box(tscr *s, int x, int y, int w, int h, int fg, int dbl, const char *title, int tfg)
{
    const char *tl = dbl ? "╔" : "┌", *tr = dbl ? "╗" : "┐", *bl = dbl ? "╚" : "└", *br = dbl ? "╝" : "┘",
               *hz = dbl ? "═" : "─", *vt = dbl ? "║" : "│";
    int j;
    if (w < 2 || h < 2) return;
    tscr_fill(s, x + 1, y, w - 2, 1, hz, fg, -1);
    tscr_fill(s, x + 1, y + h - 1, w - 2, 1, hz, fg, -1);
    for (j = 1; j < h - 1; j++) { tscr_put(s, x, y + j, vt, fg, -1); tscr_put(s, x + w - 1, y + j, vt, fg, -1); }
    tscr_put(s, x, y, tl, fg, -1); tscr_put(s, x + w - 1, y, tr, fg, -1);
    tscr_put(s, x, y + h - 1, bl, fg, -1); tscr_put(s, x + w - 1, y + h - 1, br, fg, -1);
    if (title) {
        tscr_put(s, x + 2, y, " ", tfg, -1);
        tscr_put(s, x + 3, y, title, tfg, -1);
        tscr_put(s, x + 3 + tscr_len(title), y, " ", tfg, -1);
    }
}

void tscr_dot(tscr *s, int x, int y, int col)
{
    if (x >= 0 && x < TSCR_COLS && y >= 0 && y < TSCR_ROWS * 2) s->px[y * TSCR_COLS + x] = (uint8_t)(col & 15);
}

void tscr_flush(tscr *s)
{
    int x, y;
    for (y = 0; y < TSCR_ROWS; y++)
        for (x = 0; x < TSCR_COLS; x++) {
            uint8_t top = s->px[2 * y * TSCR_COLS + x], bot = s->px[(2 * y + 1) * TSCR_COLS + x];
            tscr_cell *c = &s->c[y * TSCR_COLS + x];
            if (top == TSCR_NONE && bot == TSCR_NONE) continue;
            if (top == TSCR_NONE) top = c->bg;
            if (bot == TSCR_NONE) bot = c->bg;
            if (top == bot) { c->ch = 0xDB; c->fg = top; c->bg = top; }
            else { c->ch = 0xDF; c->fg = top; c->bg = bot; }
        }
    memset(s->px, TSCR_NONE, sizeof s->px);
}

/* ---- the line ---------------------------------------------------------- */

typedef struct { char *p; size_t cap, n; int fg, bg; } wr_t;   /* fg, bg: what the terminal has now */

static void w_put(wr_t *w, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++, w->n++) if (w->n + 1 < w->cap) w->p[w->n] = s[i];
}
static void w_str(wr_t *w, const char *s) { w_put(w, s, strlen(s)); }
static void w_num(wr_t *w, int v)
{
    char b[8]; int k = 0;
    do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 7);
    while (k) w_put(w, &b[--k], 1);
}
static void w_at(wr_t *w, int y, int x) { w_str(w, "\x1b["); w_num(w, y + 1); w_put(w, ";", 1); w_num(w, x + 1); w_put(w, "H", 1); }
static void w_end(wr_t *w) { if (w->cap) w->p[w->n < w->cap ? w->n : w->cap - 1] = '\0'; }

/* CGA order to ANSI order */
static const char ANSI[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static void w_reset(wr_t *w) { w_str(w, "\x1b[0m"); w->fg = TS_W; w->bg = TS_K; }

/* the colours for a cell, in as short an SGR as it takes; a blank needs
 * only its background */
static void w_attr(wr_t *w, int fg, int bg, uint8_t ch)
{
    char p[24]; size_t n = 0;
    bg &= 7;
    if (ch == ' ') fg = w->fg;
    if (fg == w->fg && bg == w->bg) return;
    if (w->fg >= 8 && fg < 8) {               /* bold off: only a reset does it everywhere */
        p[n++] = '0';
        w->fg = TS_W; w->bg = TS_K;
    } else if (fg >= 8 && w->fg < 8) p[n++] = '1';
    if ((fg & 7) != (w->fg & 7)) { if (n) p[n++] = ';'; p[n++] = '3'; p[n++] = (char)('0' + ANSI[fg & 7]); }
    if (bg != w->bg) { if (n) p[n++] = ';'; p[n++] = '4'; p[n++] = (char)('0' + ANSI[bg]); }
    w_str(w, "\x1b["); w_put(w, p, n); w_put(w, "m", 1);
    w->fg = fg; w->bg = bg;
}

static void w_cell(wr_t *w, const tscr_cell *c)
{
    char ch = (char)c->ch;
    w_attr(w, c->fg, c->bg, c->ch);
    w_put(w, &ch, 1);
}

/* the last column of row y worth sending: blanks on black at the end are not */
static int row_end(const tscr *s, int y)
{
    int x = y == TSCR_ROWS - 1 ? TSCR_COLS - 1 : TSCR_COLS;  /* the bottom row: not its last cell */
    while (x > 0) {
        const tscr_cell *c = &s->c[y * TSCR_COLS + x - 1];
        if (c->ch != ' ' || (c->bg & 7) != TS_K) break;
        x--;
    }
    return x;
}

static void w_tail(wr_t *w, const tscr *s)
{
    if (w->fg != TS_W || w->bg != TS_K) w_reset(w);
    if (s->cx >= 0) w_at(w, s->cy, s->cx);
}

/* a cell in plain mode: its ASCII stand-in, a block of black as a blank */
static char plain(const tscr_cell *c)
{
    if (c->ch < 0x80) return (char)c->ch;
    if ((c->ch == 0xDB || c->ch == 0xDF || c->ch == 0xDC) && c->fg == c->bg && c->fg == TS_K) return ' ';
    return PLAIN[c->ch - 0x80];
}

static size_t plain_screen(const tscr *s, char *out, size_t cap)
{
    wr_t w = { out, cap, 0, TS_W, TS_K };
    int y, x, last = -1, end;
    for (y = 0; y < TSCR_ROWS; y++)
        for (x = 0; x < TSCR_COLS; x++) if (plain(&s->c[y * TSCR_COLS + x]) != ' ') last = y;
    for (y = 0; y <= last; y++) {
        /* 79 columns at most: the 80th would wrap the line on many terminals */
        for (end = TSCR_COLS - 1; end > 0 && plain(&s->c[y * TSCR_COLS + end - 1]) == ' '; end--) ;
        for (x = 0; x < end; x++) { char ch = plain(&s->c[y * TSCR_COLS + x]); w_put(&w, &ch, 1); }
        w_put(&w, "\r\n", 2);
    }
    w_end(&w);
    return w.n;
}

size_t tscr_ansi(const tscr *s, int ansi, char *out, size_t cap)
{
    wr_t w = { out, cap, 0, TS_W, TS_K };
    int y, x, end;
    if (!ansi) return plain_screen(s, out, cap);
    w_reset(&w);
    w_str(&w, "\x1b[2J");
    for (y = 0; y < TSCR_ROWS; y++) {
        end = row_end(s, y);
        if (end == 0) continue;
        w_at(&w, y, 0);
        for (x = 0; x < end; x++) w_cell(&w, &s->c[y * TSCR_COLS + x]);
    }
    w_tail(&w, s);
    w_end(&w);
    return w.n;
}

#define JOIN 8                    /* unchanged cells between two changes worth sending to save a move */

static int same(const tscr_cell *a, const tscr_cell *b)
{
    return a->ch == b->ch && a->fg == b->fg && (a->bg & 7) == (b->bg & 7);
}

size_t tscr_diff(const tscr *old, const tscr *now, int ansi, char *out, size_t cap)
{
    wr_t w = { out, cap, 0, TS_W, TS_K };
    int y, x, any = 0;
    if (!ansi) return plain_screen(now, out, cap);
    for (y = 0; y < TSCR_ROWS; y++) {
        int lim = y == TSCR_ROWS - 1 ? TSCR_COLS - 1 : TSCR_COLS;
        x = 0;
        while (x < lim) {
            int a, b, gap;
            const tscr_cell *row = &now->c[y * TSCR_COLS], *orow = &old->c[y * TSCR_COLS];
            while (x < lim && same(&row[x], &orow[x])) x++;
            if (x >= lim) break;
            a = x; b = x + 1;                 /* a run of changes, [a, b) */
            for (;;) {
                while (b < lim && !same(&row[b], &orow[b])) b++;
                for (gap = 0; b + gap < lim && same(&row[b + gap], &orow[b + gap]); gap++) ;
                if (b + gap < lim && gap < JOIN) { b += gap; continue; }
                break;
            }
            if (!any) { w_reset(&w); any = 1; }
            w_at(&w, y, a);
            for (x = a; x < b; x++) w_cell(&w, &row[x]);
        }
    }
    if (!any && now->cx == old->cx && now->cy == old->cy) { w_end(&w); return 0; }
    w_tail(&w, now);
    w_end(&w);
    return w.n;
}
