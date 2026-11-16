/* See dclist.h. */
#include <string.h>
#include "dclist.h"

/* a dotted address at s[0..n); 0 when it is not one (or is 0.0.0.0) */
static uint32_t addr(const char *s, size_t n)
{
    uint32_t a = 0; unsigned v = 0, digits = 0, parts = 0; size_t i;
    for (i = 0; i <= n; i++) {
        if (i < n && s[i] >= '0' && s[i] <= '9') {
            v = v * 10 + (unsigned)(s[i] - '0'); if (++digits > 3 || v > 255) return 0;
        } else if (i == n || s[i] == '.') {
            if (!digits || ++parts > 4) return 0;
            a = (a << 8) | v; v = 0; digits = 0;
        } else return 0;
    }
    return parts == 4 ? a : 0;
}

static int same(const char *s, size_t n, const char *word)
{
    size_t i;
    if (strlen(word) != n) return 0;
    for (i = 0; i < n; i++) if ((s[i] | 0x20) != word[i]) return 0;
    return 1;
}

int dclist_parse(const char *text, size_t len, nat_rewrite *out, int max)
{
    size_t at = 0; int n = 0;
    while (at < len && n < max) {
        const char *f[5]; size_t fl[5]; int nf = 0; size_t end = at, i;
        nat_rewrite r;
        while (end < len && text[end] != '\n') end++;
        i = at;
        while (i < end && nf < 5) {
            while (i < end && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) i++;
            if (i == end) break;
            f[nf] = text + i;
            while (i < end && text[i] != ' ' && text[i] != '\t' && text[i] != '\r') i++;
            fl[nf] = (size_t)(text + i - f[nf]); nf++;
        }
        at = end + 1;
        if (nf < 2 || nf > 4) continue;
        memset(&r, 0, sizeof r);
        r.from = addr(f[0], fl[0]); r.to = addr(f[1], fl[1]); r.proto = 6;
        if (!r.from || !r.to) continue;
        if (nf >= 3) {
            unsigned long v = 0; size_t k;
            for (k = 0; k < fl[2] && k < 6; k++) { if (f[2][k] < '0' || f[2][k] > '9') break; v = v * 10 + (unsigned long)(f[2][k] - '0'); }
            if (k != fl[2] || v < 1 || v > 65535) continue;
            r.port = (uint16_t)v;
        }
        if (nf == 4) {
            if (same(f[3], fl[3], "udp")) r.proto = 17;
            else if (!same(f[3], fl[3], "tcp")) continue;
        }
        out[n++] = r;
    }
    return n;
}

/* From the connection guide of the Dreamcast Live project and the list of
 * the DreamPi project (refs/dreampi-facts.md), merged. */
static const nat_open ports[] = {
    { 17, 1028, 1028 },   /* Planet Ring */
    { 17, 1285, 1285 },   /* Planet Ring (voice) */
    { 17, 7648, 7648 },   /* Planet Ring */
    { 17, 2300, 2400 },   /* Starlancer, PBA Bowling */
    { 6,  2300, 2400 },
    { 6,  47624, 47624 },
    { 17, 6500, 6500 },
    { 17, 13139, 13139 }, /* PBA Bowling */
    { 17, 3512, 3512 },   /* The Next Tetris */
    { 6,  3512, 3512 },
    { 17, 5502, 5503 },   /* the 2K sports series */
    { 17, 5656, 5656 },   /* 2K series, Internet Game Pack */
    { 6,  5011, 5011 },
    { 6,  6666, 6666 },
    { 6,  10500, 10503 }, /* Internet Game Pack */
    { 17, 6001, 6001 },   /* Ooga Booga */
    { 17, 7980, 7980 },   /* Alien Front Online */
    { 17, 9789, 9789 },   /* ChuChu Rocket */
    { 17, 9879, 9879 },   /* Dee Dee Planet */
    { 17, 13713, 13713 }, /* World Series Baseball 2K2 */
    { 17, 37171, 37171 },
    { 6,  17219, 17219 }, /* Worms World Party */
    { 17, 30099, 30099 }, /* Driving Strikers (host) */
    { 6,  37001, 37001 }, /* Floigan Bros */
};

int dclist_ports(const nat_open **table)
{
    *table = ports;
    return (int)(sizeof ports / sizeof ports[0]);
}
