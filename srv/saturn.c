/* See saturn.h. */
#include <string.h>
#include "saturn.h"

#define DSPLIT "<dataSplit>"
#define PSPLIT "<packetSplit>"
#define DSPLIT_N 11
#define PSPLIT_N 13

/* a*b*c*d at d[0..n): 1 and the address, or 0 */
static int quad_star(const char *d, size_t n, uint32_t *addr)
{
    uint32_t a = 0; size_t i = 0; int parts = 0;
    for (;;) {
        unsigned v = 0; size_t dig = 0;
        while (i < n && d[i] >= '0' && d[i] <= '9' && dig < 3) { v = v * 10u + (unsigned)(d[i++] - '0'); dig++; }
        if (!dig || v > 255) return 0;
        a = a << 8 | v; parts++;
        if (i == n) break;
        if (d[i++] != '*' || parts == 4) return 0;
    }
    if (parts != 4 || a == 0) return 0;
    *addr = a;
    return 1;
}

/* twelve digits at d[0..12) as four octets: 1 and the address, or 0 */
static int quad12(const char *d, uint32_t *addr)
{
    uint32_t a = 0; size_t i;
    for (i = 0; i < 12; i += 3) {
        unsigned v = (unsigned)(d[i] - '0') * 100u + (unsigned)(d[i + 1] - '0') * 10u + (unsigned)(d[i + 2] - '0');
        if (v > 255) return 0;
        a = a << 8 | v;
    }
    if (a == 0) return 0;
    *addr = a;
    return 1;
}

int saturn_number(const char *d, uint32_t *addr, char game[3])
{
    size_t n = strlen(d), ds;
    if (game) game[0] = 0;
    if (!strcmp(d, "0") || !strcmp(d, "00")) return SATURN_WAIT;
    if (n < 2 || d[0] != '#') return SATURN_NONE;
    d++; n--;
    /* a direct address: the star form, or exactly twelve digits */
    if (memchr(d, '*', n)) return quad_star(d, n, addr) ? SATURN_CALL : SATURN_NONE;
    ds = strspn(d, "0123456789");
    if (ds != n) return SATURN_NONE;                    /* matchmaking numbers are digits only */
    if (n == 12 && quad12(d, addr)) return SATURN_CALL;
    /* matchmaking: #0NN# waits, anything else calls; the game is the last two */
    if (game) { game[0] = n >= 2 ? d[n - 2] : d[0]; game[1] = d[n - 1]; game[2] = 0; }
    return d[0] == '0' ? SATURN_MATCH_WAIT : SATURN_MATCH_CALL;
}

void saturn_ip12(uint32_t a, char out[13])
{
    unsigned o[4] = { (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255 }; int i;
    for (i = 0; i < 4; i++) {
        out[i * 3 + 0] = (char)('0' + o[i] / 100);
        out[i * 3 + 1] = (char)('0' + o[i] / 10 % 10);
        out[i * 3 + 2] = (char)('0' + o[i] % 10);
    }
    out[12] = 0;
}

/* the string value of JSON key `key` in s[0..n): into v (cut to cap), or 0 */
static int json_str(const char *s, size_t n, const char *key, char *v, size_t cap)
{
    size_t kn = strlen(key), i, j;
    for (i = 0; i + kn + 2 <= n; i++) {
        if (s[i] != '"' || memcmp(s + i + 1, key, kn) != 0 || s[i + 1 + kn] != '"') continue;
        j = i + 1 + kn + 1;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j >= n || s[j] != ':') continue;
        j++;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j >= n || s[j] != '"') continue;
        j++;
        for (i = 0; j < n && s[j] != '"' && i + 1 < cap; j++) v[i++] = s[j];
        v[i] = 0;
        return j < n && s[j] == '"';
    }
    return 0;
}

int saturn_match_parse(const char *json, size_t len, const char *want, uint32_t *addr, uint16_t *port)
{
    char st[32], op[40]; unsigned long p; size_t i;
    if (!json_str(json, len, "status", st, sizeof st) || strcmp(st, want) != 0) return 0;
    if (!json_str(json, len, "opponent ip_address", op, sizeof op)) return 0;
    if (strspn(op, "0123456789") != 12 || op[12] != ' ') return 0;
    if (!quad12(op, addr)) return 0;
    for (i = 13, p = 0; op[i] >= '0' && op[i] <= '9'; i++) p = p * 10u + (unsigned long)(op[i] - '0');
    if (op[i] != 0 || p < 1 || p > 65535) return 0;
    *port = (uint16_t)p;
    return 1;
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
    if (newest < r->expect || newest - r->expect > SATURN_JUMP_MAX) return 0;
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
