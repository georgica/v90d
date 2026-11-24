/* See tart.h. */
#include <math.h>
#include <string.h>
#include "tart.h"

static int rnd(double v) { return (int)floor(v + 0.5); }

static void ring(tscr *s, int cx, int cy, int r, int front)
{
    int k;
    for (k = 0; k < 240; k++) {                   /* every 1.5 degrees */
        double t = k * 1.5 * 3.14159265358979 / 180.0;
        int x = rnd(cx + cos(t) * (r + 6)), y = rnd(cy + sin(t) * 3.2 - cos(t) * 1.4);
        if ((sin(t) > 0) != front) continue;
        tscr_dot(s, x, y, cos(t) < -0.3 ? TS_YL : sin(t) > 0.7 ? TS_Y : TS_YL);
    }
}

void tart_planet(tscr *s, int cx, int cy, int r)
{
    int x, y;
    ring(s, cx, cy, r, 0);
    for (y = cy - r; y <= cy + r; y++)
        for (x = cx - r; x <= cx + r; x++) {
            double dx = x - cx, dy = y - cy, d = sqrt(dx * dx + dy * dy);
            double lx = x - (cx - r * 0.45), ly = y - (cy - r * 0.45), l = sqrt(lx * lx + ly * ly) / (r * 1.6);
            int col, land;
            if (d > r + 0.3) continue;
            col = l < 0.25 ? TS_WH : l < 0.45 ? TS_LC : l < 0.75 ? TS_C : TS_B;
            /* JavaScript's % keeps the sign: so does C99's */
            land = ((x * 7 + y * 13) % 11 < 3) && dx > -r * 0.6 && fabs(dy - dx * 0.4) < r * 0.55 && d < r - 1;
            if (land) col = l < 0.5 ? TS_LG : TS_G;
            tscr_dot(s, x, y, col);
        }
    ring(s, cx, cy, r, 1);
}

/* 4x7 letters (I is 3 wide) */
static const char *glyph(char ch)
{
    switch (ch) {
    case 'N': return "X..X" "XX.X" "XX.X" "X.XX" "X.XX" "X..X" "X..X";
    case 'E': return "XXXX" "X..." "X..." "XXX." "X..." "X..." "XXXX";
    case 'T': return "XXXX" ".XX." ".XX." ".XX." ".XX." ".XX." ".XX.";
    case 'S': return ".XXX" "X..." "X..." ".XX." "...X" "...X" "XXX.";
    case 'A': return ".XX." "X..X" "X..X" "XXXX" "X..X" "X..X" "X..X";
    case 'L': return "X..." "X..." "X..." "X..." "X..." "X..." "XXXX";
    case 'G': return ".XXX" "X..." "X..." "X.XX" "X..X" "X..X" ".XXX";
    default:  return "XXX" ".X." ".X." ".X." ".X." ".X." "XXX";      /* I */
    }
}

int tart_wordmark(tscr *s, int x0, int y0)
{
    static const uint8_t ramp[7] = { TS_WH, TS_WH, TS_LC, TS_LC, TS_C, TS_C, TS_B };
    const char *w = "NETSTALGIA";
    int x = x0, pass, r, i;
    for (; *w; w++) {
        const char *g = glyph(*w);
        int gw = (int)strlen(g) / 7;
        for (pass = 0; pass < 2; pass++)          /* the shadow first, the letter over it */
            for (r = 0; r < 7; r++)
                for (i = 0; i < gw; i++)
                    if (g[r * gw + i] == 'X') {
                        if (pass == 0) tscr_dot(s, x + i + 1, y0 + r + 1, TS_DK);
                        else tscr_dot(s, x + i, y0 + r, ramp[r]);
                    }
        x += gw + 1;
    }
    return x - x0 - 1;
}
