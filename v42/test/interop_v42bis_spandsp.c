/* V.42 bis interop: our encoder into spandsp's decoder and spandsp's encoder
 * into our decoder, over several data kinds, dictionary sizes, string
 * lengths, mode policies and flush patterns. Host only. */
#include <string.h>
#include "test.h"
#include "v42_comp.h"
#include "peer_spandsp_v42.h"

static uint32_t rs = 0xC0FFEEu;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

#define DMAX 60000
static uint8_t src[DMAX], enc[DMAX * 3], dec[DMAX + 1024], enc2[DMAX * 3];
static size_t nsrc, nenc, ndec, nenc2;

enum { F_RANDOM, F_TEXT, F_RUNS, F_ESC, F_MIXED, F_KINDS };
static const char *kind_name[F_KINDS] = { "random", "text", "runs", "escape", "mixed" };

static void fill(size_t len, int kind)
{
    static const char *words[16] = {
        "the", "modem", "carrier", "signal", "error", "frame", "window", "answer",
        "originate", "line", "data", "octet", "phase", "symbol", "rate", "retrain" };
    size_t i = 0;
    nsrc = len;
    while (i < len) {
        int k = kind == F_MIXED ? (int)((i / 5000) % 4) : kind;
        if (k == F_RANDOM) src[i++] = (uint8_t)(rnd32() >> 9);
        else if (k == F_TEXT) {
            const char *s = words[(rnd32() >> 7) & 15];
            while (*s && i < len) src[i++] = (uint8_t)*s++;
            if (i < len) src[i++] = ((rnd32() >> 5) % 11) ? ' ' : '\n';
        } else if (k == F_RUNS) {
            size_t n = 1 + (rnd32() >> 8) % 900;
            uint8_t c = (uint8_t)(rnd32() >> 11);
            while (n-- && i < len) src[i++] = c;
        } else {
            static const uint8_t e[6] = { 0, 51, 102, 153, 204, 255 };
            src[i++] = e[(rnd32() >> 9) % 6];
        }
    }
}

static void sp_enc_out(void *user, const uint8_t *buf, int len)
{
    (void)user;
    if (len > 0 && nenc2 + (size_t)len <= sizeof enc2) { memcpy(enc2 + nenc2, buf, (size_t)len); nenc2 += (size_t)len; }
}

static void sp_dec_out(void *user, const uint8_t *buf, int len)
{
    (void)user;
    if (len > 0 && ndec + (size_t)len <= sizeof dec) memcpy(dec + ndec, buf, (size_t)len);
    if (len > 0) ndec += (size_t)len;
}

/* our encoder over src into enc, flushing after random-sized pieces when
 * flush_every is nonzero */
static void our_encode(int p1, int p2, int policy, int flush_every)
{
    static v42comp_enc_t e;
    size_t pos = 0;
    nenc = 0;
    CHECK_EQ_I(v42comp_enc_init(&e, p1, p2), 0);
    v42comp_enc_policy(&e, policy);
    while (pos < nsrc) {
        size_t c = flush_every ? 1 + (rnd32() >> 8) % (size_t)flush_every : nsrc;
        size_t end = pos + c < nsrc ? pos + c : nsrc;
        while (pos < end) pos += v42comp_encode(&e, src + pos, end - pos, enc, sizeof enc, &nenc);
        if (flush_every) CHECK_EQ_I(v42comp_flush(&e, enc, sizeof enc, &nenc), 1);
    }
    CHECK_EQ_I(v42comp_flush(&e, enc, sizeof enc, &nenc), 1);
}

static void sp_encode(int p1, int p2, int mode, int flush_every)
{
    size_t pos = 0;
    nenc2 = 0;
    CHECK_EQ_I(sp_v42bis_open(p1, p2, mode, sp_enc_out, sp_dec_out, NULL), 0);
    while (pos < nsrc) {
        size_t c = flush_every ? 1 + (rnd32() >> 8) % (size_t)flush_every : 4096;
        if (c > nsrc - pos) c = nsrc - pos;
        sp_v42bis_compress(src + pos, (int)c);
        pos += c;
        if (flush_every) sp_v42bis_compress_flush();
    }
    sp_v42bis_compress_flush();
}

static int ours_to_spandsp(int p1, int p2, int policy, int flush_every)
{
    size_t pos = 0;
    our_encode(p1, p2, policy, flush_every);
    ndec = 0;
    CHECK_EQ_I(sp_v42bis_open(p1, p2, 0, sp_enc_out, sp_dec_out, NULL), 0);
    while (pos < nenc) {
        size_t c = 1 + (rnd32() >> 8) % 300;
        if (c > nenc - pos) c = nenc - pos;
        sp_v42bis_decompress(enc + pos, (int)c);
        pos += c;
    }
    sp_v42bis_decompress_flush();
    return ndec == nsrc && memcmp(dec, src, nsrc) == 0;
}

static int spandsp_to_ours(int p1, int p2, int mode, int flush_every)
{
    static v42comp_dec_t x;
    size_t pos = 0;
    int rc = 0;
    sp_encode(p1, p2, mode, flush_every);
    ndec = 0;
    CHECK_EQ_I(v42comp_dec_init(&x, p1, p2), 0);
    while ((pos < nenc2 || v42comp_dec_pending(&x)) && rc == 0) {
        size_t used = 0, c = 1 + (rnd32() >> 8) % 300, before = ndec;
        if (c > nenc2 - pos) c = nenc2 - pos;
        rc = v42comp_decode(&x, enc2 + pos, c, &used, dec, sizeof dec, &ndec);
        pos += used;
        if (used == 0 && ndec == before) break;
    }
    CHECK_EQ_I(rc, 0);
    return ndec == nsrc && memcmp(dec, src, nsrc) == 0;
}

int main(void)
{
    static const int parm[4][2] = { { 512, 6 }, { 1024, 32 }, { 2048, 250 }, { 2048, 6 } };
    int kind, pi, mode, fl;
    unsigned long in_total[2] = { 0, 0 }, ours_total[2] = { 0, 0 }, sp_total[2] = { 0, 0 };

    for (kind = 0; kind < F_KINDS; kind++) {
        int ok_os = 0, ok_so = 0, n = 0;
        for (pi = 0; pi < 4; pi++) {
            int p1 = parm[pi][0], p2 = parm[pi][1];
            if (p1 > V42COMP_MAX_CODEWORDS) continue;
            for (mode = 0; mode < 3; mode++) {
                for (fl = 0; fl < 3; fl++) {
                    static const int fe[3] = { 0, 2000, 40 };
                    int a, b;
                    fill(20000 + (size_t)(rnd32() >> 8) % 30000, kind);
                    a = ours_to_spandsp(p1, p2, mode, fe[fl]);
                    b = spandsp_to_ours(p1, p2, mode, fe[fl]);
                    CHECK(a); CHECK(b);
                    ok_os += a; ok_so += b; n++;
                    if (fl == 0 && mode < 2) {
                        /* same input, no flush until the end: compare what
                         * the two encoders make of it. The streams are not
                         * octet-identical (spandsp sends the first character
                         * before its switch to compressed mode, we switch
                         * first), so only the sizes are compared. */
                        our_encode(p1, p2, mode, 0);
                        sp_encode(p1, p2, mode, 0);
                        in_total[mode] += nsrc; ours_total[mode] += nenc; sp_total[mode] += nenc2;
                    }
                }
            }
        }
        printf("  v42bis %-7s ours->spandsp %d/%d  spandsp->ours %d/%d\n",
               kind_name[kind], ok_os, n, ok_so, n);
    }
    for (mode = 0; mode < 2; mode++) {
        printf("  v42bis %s: %lu chars -> ours %lu octets, spandsp %lu octets\n",
               mode ? "always compressed" : "dynamic mode     ",
               in_total[mode], ours_total[mode], sp_total[mode]);
    }
    /* in compressed mode the algorithm is fixed by the Recommendation, so
     * the sizes must agree closely; the mode decision is our own (7.8) and
     * must not do worse than a few percent against spandsp's */
    CHECK(ours_total[1] <= sp_total[1] + sp_total[1] / 100);
    CHECK(sp_total[1] <= ours_total[1] + ours_total[1] / 100);
    CHECK(ours_total[0] <= sp_total[0] + sp_total[0] / 20);
    T_REPORT("interop_v42bis_spandsp");
}
