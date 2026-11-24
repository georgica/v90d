/* v90d/term - the terminal menu's screen: 80x25 cells of CP437 characters
 * in the 16 CGA colours, a plane of half-block pixels (two to a cell, 80x50)
 * for the art, and what goes down the line for it: ANSI (the whole screen,
 * or only what changed since the last one) or plain ASCII for a terminal
 * without colour.
 *
 * Text comes in as UTF-8 and is kept as CP437, one octet a cell; what CP437
 * lacks, or would put a control code on the line (0x10 is a triangle on the
 * screen but some terminals act on it), becomes an ASCII stand-in.
 *
 * Shared by v90d (srv/termsvc.c) and Netstalgia's nsdiald. Portable C99, no
 * allocation, no globals. Ours; written for this project. */
#ifndef V90D_TSCR_H
#define V90D_TSCR_H
#include <stddef.h>
#include <stdint.h>

#define TSCR_COLS 80
#define TSCR_ROWS 25
#define TSCR_NONE 0xFF            /* a pixel not set */

/* the CGA colours, in CGA order */
enum { TS_K, TS_B, TS_G, TS_C, TS_R, TS_M, TS_Y, TS_W,
       TS_DK, TS_LB, TS_LG, TS_LC, TS_LR, TS_LM, TS_YL, TS_WH };

typedef struct { uint8_t ch, fg, bg; } tscr_cell;   /* ch: CP437; bg 0..7 on the line */

typedef struct {
    tscr_cell c[TSCR_COLS * TSCR_ROWS];
    uint8_t   px[TSCR_COLS * TSCR_ROWS * 2];         /* colour or TSCR_NONE */
    int       cx, cy;             /* where the cursor is left; cx < 0: anywhere */
} tscr;

/* the CP437 octet for a Unicode code point: the character itself, an ASCII
 * stand-in, or '?' (control codes included) */
uint8_t tscr_cp437(uint32_t cp);
/* how many characters (cells) the UTF-8 text takes */
int     tscr_len(const char *utf8);

/* all blank (white on black), no pixels, the cursor hidden */
void tscr_clear(tscr *s);
/* text at column x, row y (0-based), clipped at the edges; bg < 0 keeps
 * each cell's background. Returns the cells written. */
int  tscr_put(tscr *s, int x, int y, const char *utf8, int fg, int bg);
/* a w x h block of the one character */
void tscr_fill(tscr *s, int x, int y, int w, int h, const char *utf8, int fg, int bg);
/* row y as a bar: blanks in fg/bg, left at its start, right at its end */
void tscr_bar(tscr *s, int y, const char *left, const char *right, int fg, int bg);
/* a frame, single or double line, with an optional title in tfg */
void tscr_box(tscr *s, int x, int y, int w, int h, int fg, int dbl, const char *title, int tfg);
/* a pixel: x 0..79, y 0..49 */
void tscr_dot(tscr *s, int x, int y, int col);
/* the pixels into the cells (an upper half block, the top pixel's colour on
 * the bottom one's; a cell's own background where one is missing), then
 * the pixel plane is empty again */
void tscr_flush(tscr *s);

/* The whole screen for the line into out[0..cap), NUL terminated. ansi: ESC
 * [2J, then each row that has anything placed and written in as few colour
 * changes as it takes (the bottom row stops at column 79, so the screen does
 * not scroll), the cursor last. Plain (ansi 0): no escapes at all, the
 * colours gone, box and block characters drawn in ASCII, rows ended by CR
 * LF, trailing blanks and empty rows left off. Returns the length it needs:
 * when that is cap or more, out holds what fitted. */
size_t tscr_ansi(const tscr *s, int ansi, char *out, size_t cap);
/* Only what changed from old to now (nearby changes on a row joined into
 * one run), and the cursor; 0 when nothing did. Plain: the whole screen. */
size_t tscr_diff(const tscr *old, const tscr *now, int ansi, char *out, size_t cap);

#endif
