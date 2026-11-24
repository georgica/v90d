/* tscr.h: the terminal menu's 80x25 screen: cells, half-block pixels, CP437,
 * and what goes down the line (ANSI, the changes only, or plain ASCII). */
#include <string.h>
#include "test.h"
#define CHECKF(c, ...) do { t_checks++; if (!(c)) { t_fails++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#include "tscr.h"

static char out[16384];

int main(void)
{
    static tscr s, o;
    size_t n;

    /* CP437: the box and block characters, accents, ASCII stand-ins, '?' */
    CHECK(tscr_cp437(0x2554) == 0xC9);          /* ╔ */
    CHECK(tscr_cp437(0x2588) == 0xDB);          /* █ */
    CHECK(tscr_cp437(0x2580) == 0xDF);          /* ▀ */
    CHECK(tscr_cp437(0x00E9) == 0x82);          /* é */
    CHECK(tscr_cp437(0x2019) == '\'');          /* ’ */
    CHECK(tscr_cp437(0x2026) == '.');           /* … */
    CHECK(tscr_cp437(0x00A0) == ' ');
    CHECK(tscr_cp437(0x20AC) == '?');           /* € is not in CP437 */
    CHECK(tscr_cp437('A') == 'A');
    CHECK(tscr_cp437(0x07) == '?');             /* no control codes on the screen */

    /* put: UTF-8 in, one cell per character, clipped at the edges */
    tscr_clear(&s);
    CHECK(s.c[0].ch == ' ' && s.c[0].fg == TS_W && s.c[0].bg == TS_K);
    CHECK(tscr_put(&s, 78, 0, "╔═╗", TS_YL, -1) == 2);
    CHECK(s.c[78].ch == 0xC9 && s.c[79].ch == 0xCD && s.c[78].fg == TS_YL && s.c[78].bg == TS_K);
    CHECK(tscr_put(&s, 0, 25, "x", TS_W, -1) == 0);
    CHECK(tscr_put(&s, -1, 1, "ab", TS_W, TS_B) == 1 && s.c[80].ch == 'b' && s.c[80].bg == TS_B);
    tscr_put(&s, 0, 2, "\xff\xfe""ok", TS_W, -1);     /* broken UTF-8: '?' per bad byte */
    CHECK(s.c[160].ch == '?' && s.c[161].ch == '?' && s.c[162].ch == 'o');

    /* bar: a full-width row, left and right text */
    tscr_clear(&s);
    tscr_bar(&s, 0, " NETSTALGIA", "0:00 ", TS_WH, TS_B);
    CHECK(s.c[0].bg == TS_B && s.c[79].bg == TS_B && s.c[1].ch == 'N' && s.c[79].ch == ' ' && s.c[78].ch == '0' && s.c[75].ch == '0');

    /* box: corners, sides, title */
    tscr_clear(&s);
    tscr_box(&s, 0, 0, 10, 3, TS_LB, 1, "Hi", TS_WH);
    CHECK(s.c[0].ch == 0xC9 && s.c[9].ch == 0xBB && s.c[2 * 80].ch == 0xC8 && s.c[2 * 80 + 9].ch == 0xBC);
    CHECK(s.c[80].ch == 0xBA && s.c[89].ch == 0xBA && s.c[3].ch == 'H' && s.c[3].fg == TS_WH);
    tscr_box(&s, 20, 0, 5, 3, TS_W, 0, NULL, 0);
    CHECK(s.c[20].ch == 0xDA && s.c[21].ch == 0xC4 && s.c[100].ch == 0xB3);

    /* pixels: two per cell, ▀ with the top colour on the bottom one */
    tscr_clear(&s);
    tscr_put(&s, 0, 1, " ", TS_W, TS_B);
    tscr_dot(&s, 0, 0, TS_WH); tscr_dot(&s, 0, 1, TS_C);     /* two colours: ▀ */
    tscr_dot(&s, 1, 0, TS_LG); tscr_dot(&s, 1, 1, TS_LG);    /* one colour: █ */
    tscr_dot(&s, 2, 0, TS_YL);                               /* top only: on the cell's own bg */
    tscr_dot(&s, 0, 3, TS_R);                                /* bottom only, on blue */
    tscr_dot(&s, 80, 0, TS_R); tscr_dot(&s, 0, 50, TS_R); tscr_dot(&s, -1, 0, TS_R);
    tscr_flush(&s);
    CHECK(s.c[0].ch == 0xDF && s.c[0].fg == TS_WH && s.c[0].bg == TS_C);
    CHECK(s.c[1].ch == 0xDB && s.c[1].fg == TS_LG);
    CHECK(s.c[2].ch == 0xDF && s.c[2].fg == TS_YL && s.c[2].bg == TS_K);
    CHECK(s.c[80].ch == 0xDF && s.c[80].fg == TS_B && s.c[80].bg == TS_R);
    CHECK(s.c[3].ch == ' ');
    tscr_flush(&s);                                          /* the pixels are gone once flushed */
    CHECK(s.c[0].ch == 0xDF);

    /* ANSI: clear, then each row placed; nothing for black blanks at the end */
    tscr_clear(&s);
    tscr_put(&s, 0, 0, "Hi", TS_W, -1);
    s.cx = 2; s.cy = 0;
    n = tscr_ansi(&s, 1, out, sizeof out);
    CHECKF(n == strlen(out) && !strcmp(out, "\x1b[0m\x1b[2J\x1b[1;1HHi\x1b[1;3H"), "%s", out);

    /* colours: bright foregrounds by bold, CGA order to ANSI order, one SGR per change */
    tscr_clear(&s);
    tscr_put(&s, 0, 0, "A", TS_WH, -1);
    tscr_put(&s, 1, 0, "B", TS_YL, -1);
    tscr_put(&s, 2, 0, "C", TS_B, -1);
    tscr_put(&s, 3, 0, "D", TS_K, TS_C);
    tscr_put(&s, 4, 0, "E", TS_K, TS_C);
    s.cx = -1;
    tscr_ansi(&s, 1, out, sizeof out);
    CHECKF(strstr(out, "\x1b[1;1H\x1b[1mA\x1b[33mB\x1b[0;34mC\x1b[30;46mDE\x1b[0m"), "%s", out);
    CHECK(!strcmp(out + strlen(out) - 6, "DE\x1b[0m"));    /* back to plain, no cursor move when hidden */

    /* a bar is not trimmed; the bottom row stops at column 79 (no scroll) */
    tscr_clear(&s);
    tscr_bar(&s, 24, " keys", "", TS_K, TS_C);
    s.cx = -1;
    tscr_ansi(&s, 1, out, sizeof out);
    {
        const char *r = strstr(out, "\x1b[25;1H");
        int vis = 0;
        for (r = r ? r + 7 : out + strlen(out); *r; r++) {
            if (*r == 0x1b) { while (*r && *r != 'm') r++; if (!*r) break; continue; }
            vis++;
        }
        CHECKF(vis == 79, "%d", vis);
    }

    /* the changes only: one cell moved, no clear */
    tscr_clear(&o);
    tscr_put(&o, 0, 3, "abc", TS_W, -1);
    s = o;
    tscr_put(&s, 1, 3, "X", TS_K, TS_C);
    s.cx = -1;
    n = tscr_diff(&o, &s, 1, out, sizeof out);
    CHECKF(!strcmp(out, "\x1b[0m\x1b[4;2H\x1b[30;46mX\x1b[0m"), "%s", out);
    o = s;
    n = tscr_diff(&o, &s, 1, out, sizeof out);
    CHECK(n == 0 && out[0] == 0);
    /* changes far apart on one row: two moves; close together: one run */
    s = o;
    tscr_put(&s, 0, 5, "a", TS_W, -1); tscr_put(&s, 70, 5, "b", TS_W, -1);
    tscr_diff(&o, &s, 1, out, sizeof out);
    CHECKF(!strcmp(out, "\x1b[0m\x1b[6;1Ha\x1b[6;71Hb"), "%s", out);
    s = o;
    tscr_put(&s, 0, 5, "a", TS_W, -1); tscr_put(&s, 3, 5, "b", TS_W, -1);
    tscr_diff(&o, &s, 1, out, sizeof out);
    CHECKF(!strcmp(out, "\x1b[0m\x1b[6;1Ha  b"), "%s", out);

    /* plain ASCII: no escapes, the box and block characters drawn in ASCII,
       rows by CR LF, trailing blanks dropped */
    tscr_clear(&s);
    tscr_box(&s, 0, 0, 4, 3, TS_LB, 1, NULL, 0);
    tscr_put(&s, 0, 4, "█▀░▒▓►●■□·", TS_WH, -1);
    tscr_put(&s, 0, 5, "é", TS_W, -1);
    tscr_put(&s, 4, 0, "─┼═", TS_W, -1);                   /* the single line is '-', a cross '+' */
    n = tscr_ansi(&s, 0, out, sizeof out);
    CHECK(!strchr(out, 0x1b));
    CHECKF(!strcmp(out, "+==+-+=\r\n|  |\r\n+==+\r\n\r\n##.:#>*xo-\r\ne\r\n"), "%s", out);   /* trailing empty rows dropped */
    /* without colour, a diff is the whole screen again */
    CHECK(tscr_diff(&o, &s, 0, out, sizeof out) == n);

    /* too small a buffer: the length it needed, the buffer terminated */
    tscr_clear(&s);
    tscr_bar(&s, 0, "x", "y", TS_WH, TS_B);
    n = tscr_ansi(&s, 1, out, 10);
    CHECK(n > 10 && strlen(out) == 9);

    T_REPORT("tscr");
}
