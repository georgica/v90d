/* See saturn.h. */
#include <string.h>
#include "saturn.h"

#define DSPLIT "<dataSplit>"
#define PSPLIT "<packetSplit>"
#define DSPLIT_N 11
#define PSPLIT_N 13

int saturn_number(const char *d, uint32_t *addr)
{
    uint32_t a = 0; size_t n = strlen(d), i; int parts = 0;
    if (!strcmp(d, "0") || !strcmp(d, "00")) return SATURN_WAIT;
    if (n < 2 || d[0] != '#') return SATURN_NONE;
    d++; n--;
    if (n == 12 && strspn(d, "0123456789") == 12) {
        for (i = 0; i < 12; i += 3) {
            unsigned v = (unsigned)(d[i] - '0') * 100u + (unsigned)(d[i + 1] - '0') * 10u + (unsigned)(d[i + 2] - '0');
            if (v > 255) return SATURN_NONE;
            a = a << 8 | v;
        }
    } else {
        i = 0;
        for (;;) {
            unsigned v = 0; size_t dig = 0;
            while (i < n && d[i] >= '0' && d[i] <= '9' && dig < 3) { v = v * 10u + (unsigned)(d[i++] - '0'); dig++; }
            if (!dig || v > 255) return SATURN_NONE;
            a = a << 8 | v; parts++;
            if (i == n) break;
            if (d[i++] != '*' || parts == 4) return SATURN_NONE;
        }
        if (parts != 4) return SATURN_NONE;
    }
    if (a == 0) return SATURN_NONE;
    *addr = a;
    return SATURN_CALL;
}

int saturn_control(const uint8_t *dg, size_t len)
{
    if (len == 10 && memcmp(dg, SATURN_PING, 10) == 0) return 1;
    if (len == 10 && (memcmp(dg, SATURN_PONG, 10) == 0 || memcmp(dg, SATURN_OPEN, 10) == 0)) return 2;
    return 0;
}

void saturn_tx_init(saturn_tx *t) { memset(t, 0, sizeof *t); }
void saturn_rx_init(saturn_rx *r) { r->expect = 0; }

static size_t put_dec(uint8_t *out, uint32_t v)
{
    char tmp[10]; size_t n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    for (i = 0; i < n; i++) out[i] = (uint8_t)tmp[n - 1 - i];
    return n;
}

size_t saturn_pack(saturn_tx *t, const uint8_t *oct, size_t n, uint8_t *out)
{
    size_t at = 0; uint32_t k, have = t->seq < SATURN_KEEP - 1 ? t->seq : SATURN_KEEP - 1;
    unsigned slot = (unsigned)(t->seq % SATURN_KEEP);
    if (n > SATURN_REC_MAX) n = SATURN_REC_MAX;
    memcpy(t->rec[slot], oct, n); t->len[slot] = (uint16_t)n;
    for (k = 0; k <= have; k++) {
        uint32_t q = t->seq - k; unsigned s = (unsigned)(q % SATURN_KEEP);
        if (k) { memcpy(out + at, PSPLIT, PSPLIT_N); at += PSPLIT_N; }
        memcpy(out + at, t->rec[s], t->len[s]); at += t->len[s];
        memcpy(out + at, DSPLIT, DSPLIT_N); at += DSPLIT_N;
        at += put_dec(out + at, q);
    }
    t->seq++;
    return at;
}

size_t saturn_unpack(saturn_rx *r, const uint8_t *dg, size_t len, uint8_t *out, size_t cap)
{
    struct { const uint8_t *p; size_t n; uint32_t seq; } rec[8];
    int nrec = 0, i; size_t at = 0, got = 0; uint32_t newest;

    /* records from the front: octets, the mark, a number, and the mark
     * between records. The octets are not escaped, so a record ends at the
     * first place that reads as mark, number, and record mark or the end. */
    while (at < len && nrec < 8) {
        size_t d = at, e; uint64_t v; size_t dig;
        for (;;) {
            while (d + DSPLIT_N <= len && memcmp(dg + d, DSPLIT, DSPLIT_N) != 0) d++;
            if (d + DSPLIT_N > len) return 0;
            e = d + DSPLIT_N; v = 0; dig = 0;
            while (e < len && dg[e] >= '0' && dg[e] <= '9' && dig < 10) { v = v * 10u + (uint64_t)(dg[e++] - '0'); dig++; }
            if (dig && v <= 0xFFFFFFFFu && (e == len || (e + PSPLIT_N <= len && memcmp(dg + e, PSPLIT, PSPLIT_N) == 0))) break;
            d++;
        }
        rec[nrec].p = dg + at; rec[nrec].n = d - at; rec[nrec].seq = (uint32_t)v; nrec++;
        at = e == len ? len : e + PSPLIT_N;
        if (e != len && at == len) return 0;        /* a record mark with nothing behind it */
    }
    if (!nrec) return 0;
    newest = rec[0].seq;
    if (newest < r->expect) return 0;
    for (;;) {
        int use = 0, last;
        for (i = 0; i < nrec; i++) if (rec[i].seq == r->expect) { use = i; break; }
        if (i == nrec) use = 0;
        last = rec[use].seq == newest;
        if (rec[use].n) {
            size_t n = rec[use].n < cap - got ? rec[use].n : cap - got;
            memcpy(out + got, rec[use].p, n); got += n;
        }
        r->expect = rec[use].seq + 1u;
        if (last) break;
    }
    return got;
}
