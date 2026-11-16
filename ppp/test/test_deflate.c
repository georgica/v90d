/* PPP Deflate (ppp_deflate.c): the compressor against the decompressor over
 * long packet sequences and every window size, the decompressor against
 * packets made by an independent implementation (zlib, recorded below; the
 * live comparison both ways is test/zlib_check.py), and garbage.
 *
 *   test_deflate          everything
 *   test_deflate quick    shorter sequences (used under qemu) */
#include <string.h>
#include "test.h"
#include "ppp_deflate.h"
#include "gen.h"

static ppp_deflate_enc_t E;
static ppp_deflate_dec_t D;

/* ---- packets recorded from zlib (raw deflate, Z_SYNC_FLUSH per packet,
 * the last four octets removed), one history across each group ---- */
static const char *const z_plain[3] = {
    "!hello hello hello hello, world", "!hello again, world; hello hello", NULL /* '!' then 0..63 */
};
static const uint8_t z_fix0[] = { 0x52, 0xcc, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc0, 0x20, 0x75, 0x14, 0xca, 0xf3, 0x8b, 0x72, 0x52, 0x00, 0x00 };
static const uint8_t z_fix1[] = { 0x52, 0x84, 0x08, 0x25, 0xa6, 0x27, 0x66, 0xe6, 0x41, 0x85, 0xac, 0x91, 0x95, 0x01, 0x00 };
static const uint8_t z_fix2[] = {
    0x52, 0x64, 0x60, 0x64, 0x62, 0x66, 0x61, 0x65, 0x63, 0xe7, 0xe0, 0xe4, 0xe2, 0xe6, 0xe1, 0xe5, 0xe3, 0x17, 0x10, 0x14, 0x12, 0x16,
    0x11, 0x15, 0x13, 0x97, 0x90, 0x94, 0x92, 0x96, 0x91, 0x95, 0x93, 0x57, 0x50, 0x54, 0x52, 0x56, 0x51, 0x55, 0x53, 0xd7, 0xd0, 0xd4,
    0xd2, 0xd6, 0xd1, 0xd5, 0xd3, 0x37, 0x30, 0x34, 0x32, 0x36, 0x31, 0x35, 0x33, 0xb7, 0xb0, 0xb4, 0xb2, 0xb6, 0xb1, 0xb5, 0xb3, 0x07, 0x00 };
static const uint8_t z_sto0[] = {
    0x00, 0x1f, 0x00, 0xe0, 0xff, 0x21, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x68, 0x65, 0x6c, 0x6c,
    0x6f, 0x20, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20, 0x77, 0x6f, 0x72, 0x6c, 0x64, 0x00 };
static const uint8_t z_sto1[] = {
    0x00, 0x20, 0x00, 0xdf, 0xff, 0x21, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x61, 0x67, 0x61, 0x69, 0x6e, 0x2c, 0x20, 0x77, 0x6f, 0x72,
    0x6c, 0x64, 0x3b, 0x20, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x00 };
/* a dynamic block: 4 x "the quick ... the fox; " then the alphabet twice */
static const uint8_t z_dyn[] = {
    0xdc, 0x8c, 0xc9, 0x11, 0x80, 0x20, 0x10, 0x04, 0x53, 0x99, 0x3c, 0x8c, 0x66, 0x91, 0x4b, 0x51, 0x56, 0x39, 0x85, 0xe8, 0x55, 0xfe,
    0x26, 0xe0, 0xaf, 0xab, 0xbb, 0x66, 0x92, 0x55, 0x38, 0xf3, 0x32, 0x3b, 0x88, 0xc0, 0xd5, 0x43, 0xf3, 0x85, 0x35, 0xef, 0x47, 0x04,
    0x17, 0x15, 0x90, 0x9e, 0xbc, 0x51, 0x6f, 0x90, 0x6c, 0x40, 0x5e, 0x0e, 0xf1, 0xb2, 0xa0, 0xe0, 0x22, 0x28, 0x0d, 0xf1, 0x8c, 0xa6,
    0x01, 0xff, 0x7d, 0x22, 0x31, 0x4b, 0xa5, 0x8d, 0x5d, 0x56, 0xb7, 0xed, 0x9e, 0x8f, 0x33, 0xc4, 0x94, 0x4b, 0xbd, 0x5a, 0xff, 0x2e,
    0x37, 0x00 };

static void test_zlib_vectors(void)
{
    uint8_t out[1600], p2[65], want[400];
    size_t i, at = 0;
    int n;

    p2[0] = '!';
    for (i = 0; i < 64; i++) p2[1 + i] = (uint8_t)i;

    /* fixed blocks; the second packet refers to the first */
    ppp_deflate_dec_init(&D);
    n = ppp_deflate_decompress(&D, z_fix0, sizeof z_fix0, out, sizeof out);
    CHECK_EQ_I(n, 31); CHECK(n == 31 && memcmp(out, z_plain[0], 31) == 0);
    n = ppp_deflate_decompress(&D, z_fix1, sizeof z_fix1, out, sizeof out);
    CHECK_EQ_I(n, 32); CHECK(n == 32 && memcmp(out, z_plain[1], 32) == 0);
    n = ppp_deflate_decompress(&D, z_fix2, sizeof z_fix2, out, sizeof out);
    CHECK_EQ_I(n, 65); CHECK(n == 65 && memcmp(out, p2, 65) == 0);

    /* the second packet without the first: its distances reach too far back */
    ppp_deflate_dec_init(&D);
    CHECK_EQ_I(ppp_deflate_decompress(&D, z_fix1, sizeof z_fix1, out, sizeof out), -1);
    /* ... but with the first one added to the history in its native form
     * (RFC 1979 section 2, "Data Expansion") it decodes */
    ppp_deflate_dec_init(&D);
    ppp_deflate_dec_add(&D, (const uint8_t *)z_plain[0], 1, (const uint8_t *)z_plain[0] + 1, 30);
    n = ppp_deflate_decompress(&D, z_fix1, sizeof z_fix1, out, sizeof out);
    CHECK_EQ_I(n, 32); CHECK(n == 32 && memcmp(out, z_plain[1], 32) == 0);

    /* stored blocks */
    ppp_deflate_dec_init(&D);
    n = ppp_deflate_decompress(&D, z_sto0, sizeof z_sto0, out, sizeof out);
    CHECK_EQ_I(n, 31); CHECK(n == 31 && memcmp(out, z_plain[0], 31) == 0);
    n = ppp_deflate_decompress(&D, z_sto1, sizeof z_sto1, out, sizeof out);
    CHECK_EQ_I(n, 32); CHECK(n == 32 && memcmp(out, z_plain[1], 32) == 0);
    /* output that does not fit is an error, and leaves the history alone */
    CHECK_EQ_I(ppp_deflate_decompress(&D, z_fix1, sizeof z_fix1, out, 31), -1);
    n = ppp_deflate_decompress(&D, z_fix1, sizeof z_fix1, out, 32);
    CHECK_EQ_I(n, 32);

    /* a dynamic block */
    for (i = 0; i < 4; i++) { memcpy(want + at, "the quick brown fox jumps over the lazy dog and the dog barks at the fox; ", 74); at += 74; }
    for (i = 0; i < 52; i++) want[at++] = (uint8_t)('a' + i % 26);
    ppp_deflate_dec_init(&D);
    n = ppp_deflate_decompress(&D, z_dyn, sizeof z_dyn, out, sizeof out);
    CHECK_EQ_I(n, (long)at); CHECK(n == (int)at && memcmp(out, want, at) == 0);

    /* every truncation of a valid packet is an error or a shorter packet, never a crash */
    for (i = 0; i < sizeof z_dyn; i++) {
        ppp_deflate_dec_init(&D);
        n = ppp_deflate_decompress(&D, z_dyn, i, out, sizeof out);
        CHECK(n == -1 || (n >= 0 && n < (int)at));
    }
    /* an empty packet is not a packet */
    ppp_deflate_dec_init(&D);
    CHECK_EQ_I(ppp_deflate_decompress(&D, z_dyn, 0, out, sizeof out), -1);
}

/* ---- our compressor against our decompressor ---- */
static unsigned long tot_in, tot_out, n_comp, n_native;

/* one packet through both; returns the compressed size (0: sent native) */
static size_t one(const uint8_t *pkt, size_t n, size_t max)
{
    static uint8_t c[PPP_DEFLATE_MAX_PKT + 64], o[PPP_DEFLATE_MAX_PKT + 8];
    static const uint8_t guard[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
    size_t cn;

    if (max > PPP_DEFLATE_MAX_PKT) max = PPP_DEFLATE_MAX_PKT;
    memcpy(c + max, guard, 4);
    /* the protocol octet apart from the rest, as ppp.c hands it over */
    cn = ppp_deflate_compress(&E, pkt, 1, pkt + 1, n - 1, c, max);
    CHECK(cn <= max);
    CHECK(memcmp(c + max, guard, 4) == 0);
    CHECK(E.last_work <= E.effort.work);
    tot_in += n;
    if (cn) {
        int dn = ppp_deflate_decompress(&D, c, cn, o, sizeof o);
        CHECK_EQ_I(dn, (long)n);
        if (dn == (int)n) CHECK(memcmp(o, pkt, n) == 0);
        tot_out += cn; n_comp++;
    } else {
        ppp_deflate_dec_add(&D, pkt, 1, pkt + 1, n - 1);
        tot_out += n; n_native++;
    }
    return cn;
}

/* a sequence of `count` packets of one kind; sizes: 0 = full, 1 = mixed, 2 = tiny */
static void sequence(int wbits, int kind, int sizes, long count, uint32_t seed)
{
    static uint8_t pkt[PPP_DEFLATE_MAX_PKT];
    gen_t g = { seed };
    long i;

    ppp_deflate_enc_init(&E, wbits, NULL);
    ppp_deflate_dec_init(&D);
    tot_in = tot_out = n_comp = n_native = 0;
    for (i = 0; i < count; i++) {
        size_t n = sizes == 0 ? 1501 : sizes == 2 ? 2 + gen_n(&g, 40) : 41 + gen_n(&g, 1461);
        int k = kind < GEN_KINDS ? kind : (int)gen_n(&g, GEN_KINDS);
        pkt[0] = 0x21;
        if (n >= 41) gen_packet(&g, k, (uint32_t)i * 1460u, pkt + 1, n - 1);
        else gen_fill(&g, k, pkt + 1, n - 1);
        /* what ppp.c allows: shorter than the native form by the two
         * octets of the sequence number */
        one(pkt, n, n > 3 ? n - 3 : 0);
    }
}

static void test_round_trips(int quick)
{
    static const char *const kn[] = { "HTML-like", "text", "binary", "random", "zeros", "mixed" };
    int w, k;

    for (k = 0; k <= GEN_KINDS; k++) {
        sequence(15, k, 0, quick ? 60 : 400, 0x1000u + (uint32_t)k);
        printf("  w 15, %-9s full packets: %lu -> %lu octets, %lu compressed, %lu native\n", kn[k], tot_in, tot_out, n_comp, n_native);
        if (k == GEN_HTML || k == GEN_TEXT) CHECK(tot_out * 2 < tot_in);
        if (k == GEN_ZERO) CHECK(tot_out * 20 < tot_in);
        if (k == GEN_RANDOM) { CHECK_EQ_I(n_comp, 0); CHECK_EQ_I(tot_out, tot_in); }
    }
    for (w = PPP_DEFLATE_MIN_WBITS; w <= PPP_DEFLATE_MAX_WBITS; w++) {
        sequence(w, GEN_KINDS, 1, quick ? 150 : 3000, 0x2000u + (uint32_t)w);
        printf("  w %2d, mixed kinds and sizes: %lu -> %lu octets\n", w, tot_in, tot_out);
        CHECK(tot_out < tot_in);
        CHECK_EQ_I(E.wbits, w);
        sequence(w, GEN_HTML, 2, quick ? 100 : 2000, 0x3000u + (uint32_t)w);
        CHECK(tot_out <= tot_in);
    }
    /* a request for more than this build has is honoured with what it has */
    ppp_deflate_enc_init(&E, 99, NULL); CHECK_EQ_I(E.wbits, PPP_DEFLATE_MAX_WBITS);
    ppp_deflate_enc_init(&E, 1, NULL); CHECK_EQ_I(E.wbits, PPP_DEFLATE_MIN_WBITS);
}

/* the window and the 16-bit positions wrap many times */
static void test_long_run(int quick)
{
    long count = quick ? 6000 : 120000;
    sequence(15, GEN_KINDS, 1, count, 77);
    printf("  %ld packets, w 15, mixed: %lu -> %lu octets (%lu native); blocks %lu dynamic, %lu fixed\n",
           count, tot_in, tot_out, n_native, (unsigned long)E.blocks_dynamic, (unsigned long)E.blocks_fixed);
    CHECK(tot_in > 60u * 65536u || quick);
    CHECK(E.blocks_dynamic > 0 && E.blocks_fixed > 0);
    sequence(9, GEN_TEXT, 1, quick ? 2000 : 30000, 78);
    CHECK(tot_out * 2 < tot_in);
}

/* a reset in the middle: both ends forget, and go on */
static void test_reset(void)
{
    static uint8_t pkt[1501], c[1501], o[1600];
    gen_t g = { 5 };
    size_t cn, cn2;
    int i;

    ppp_deflate_enc_init(&E, 15, NULL);
    ppp_deflate_dec_init(&D);
    pkt[0] = 0x21;
    gen_packet(&g, GEN_TEXT, 0, pkt + 1, 1500);
    cn = ppp_deflate_compress(&E, pkt, 1, pkt + 1, 1500, c, 1498);
    CHECK(cn > 0);
    /* the same packet again is nearly free while the history is there */
    cn2 = ppp_deflate_compress(&E, pkt, 1, pkt + 1, 1500, c, 1498);
    CHECK(cn2 > 0 && cn2 < 40);
    ppp_deflate_enc_reset(&E);
    cn2 = ppp_deflate_compress(&E, pkt, 1, pkt + 1, 1500, c, 1498);
    CHECK_EQ_I(cn2, (long)cn);
    /* a decoder that was reset as well reads it; one that was not need not
     * fail (deflate carries no check), so only the good case is tested */
    ppp_deflate_dec_reset(&D);
    CHECK_EQ_I(ppp_deflate_decompress(&D, c, cn2, o, sizeof o), 1501);
    CHECK(memcmp(o, pkt, 1501) == 0);
    for (i = 0; i < 50; i++) {
        gen_packet(&g, GEN_HTML, (uint32_t)i, pkt + 1, 1500);
        if (i == 20) { ppp_deflate_enc_reset(&E); ppp_deflate_dec_reset(&D); }
        one(pkt, 1501, 1498);
    }
}

/* the bound on the work of one call holds, and less work still round-trips */
static void test_effort(void)
{
    static uint8_t pkt[1501];
    ppp_deflate_effort_t ef;
    gen_t g = { 9 };
    unsigned long full = 0, small = 0;
    int i, pass;

    for (pass = 0; pass < 3; pass++) {
        ppp_deflate_effort_default(&ef);
        if (pass == 1) ef.work = 200;
        if (pass == 2) { ef.max_chain = 1; ef.lazy_len = 0; }
        ppp_deflate_enc_init(&E, 15, &ef);
        ppp_deflate_dec_init(&D);
        tot_in = tot_out = 0;
        g.s = 9;
        pkt[0] = 0x21;
        for (i = 0; i < 200; i++) {
            /* many candidates for every position: the worst case for the search */
            memset(pkt + 1, 'a' + i % 3, 1500);
            if (i & 1) gen_packet(&g, GEN_HTML, (uint32_t)i, pkt + 1, 1500);
            pkt[1 + gen_n(&g, 1500)] ^= 1;
            one(pkt, 1501, 1498);
        }
        if (pass == 0) full = tot_out;
        else { small = tot_out; CHECK(small >= full); CHECK(small < tot_in / 2); }
        printf("  effort %d (chain %u, work %lu): %lu -> %lu octets\n", pass, (unsigned)ef.max_chain, (unsigned long)ef.work, tot_in, tot_out);
    }
    ppp_deflate_effort_default(&ef);
    CHECK(ef.work >= 1000 && ef.max_chain >= 4);
}

/* garbage into the decompressor: an error or some output, never a crash,
 * and never more output than asked for */
static void test_garbage(int quick)
{
    static uint8_t in[1600], out[PPP_DEFLATE_MAX_PKT + 8], pkt[1501], c[1501];
    static const uint8_t guard[4] = { 1, 2, 3, 4 };
    gen_t g = { 0xBADF00D };
    long i, errors = 0, rounds = quick ? 4000 : 60000;
    size_t cn;

    ppp_deflate_dec_init(&D);
    for (i = 0; i < rounds; i++) {
        size_t n = gen_n(&g, i % 7 == 0 ? 1500 : 60), k;
        size_t max = i % 5 == 0 ? gen_n(&g, 200) : PPP_DEFLATE_MAX_PKT;
        int r;
        for (k = 0; k < n; k++) in[k] = (uint8_t)(gen_u32(&g) >> 9);
        /* steer some towards each block type */
        if (n && i % 3 == 0) in[0] = (uint8_t)((in[0] & 0xF8) | (i / 3 % 3) << 1);
        memcpy(out + max, guard, 4);
        r = ppp_deflate_decompress(&D, in, n, out, max);
        CHECK(r == -1 || (r >= 0 && (size_t)r <= max));
        CHECK(memcmp(out + max, guard, 4) == 0);
        if (r < 0) errors++;
    }
    printf("  %ld garbage packets: %ld rejected\n", rounds, errors);
    CHECK(errors > rounds * 9 / 10);

    /* a valid packet with one bit turned, every bit in turn */
    ppp_deflate_enc_init(&E, 15, NULL);
    pkt[0] = 0x21;
    gen_packet(&g, GEN_HTML, 0, pkt + 1, 600);
    cn = ppp_deflate_compress(&E, pkt, 1, pkt + 1, 600, c, 598);
    CHECK(cn > 0);
    for (i = 0; i < (long)cn * 8; i++) {
        int r;
        c[i / 8] ^= (uint8_t)(1 << (i % 8));
        ppp_deflate_dec_init(&D);
        r = ppp_deflate_decompress(&D, c, cn, out, PPP_DEFLATE_MAX_PKT);
        CHECK(r >= -1 && r <= PPP_DEFLATE_MAX_PKT);
        c[i / 8] ^= (uint8_t)(1 << (i % 8));
    }
    ppp_deflate_dec_init(&D);
    CHECK_EQ_I(ppp_deflate_decompress(&D, c, cn, out, PPP_DEFLATE_MAX_PKT), 601);
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && strcmp(argv[1], "quick") == 0;

    test_zlib_vectors();
    test_round_trips(quick);
    test_long_run(quick);
    test_reset();
    test_effort();
    test_garbage(quick);
    printf("  RAM: compressor %lu octets, decompressor %lu (window 2^%d)\n",
           (unsigned long)sizeof E, (unsigned long)sizeof D, PPP_DEFLATE_MAX_WBITS);
    T_REPORT("test_deflate");
}
