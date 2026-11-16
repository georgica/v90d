/* MPPC (ppp_mppc.c) against RFC 2118: hand-built bit patterns for every
 * class of the encoding (the patterns are written here from the RFC's
 * tables as strings of 0 and 1, and first checked against the RFC's own
 * examples), the compressor's output read back by the decompressor and by
 * a second, strict reader in this file, history wrap, expansion, garbage.
 *
 *   test_mppc          everything
 *   test_mppc quick    shorter sequences (used under qemu) */
#include <string.h>
#include "test.h"
#include "ppp_mppc.h"
#include "gen.h"

static ppp_mppc_enc_t E;
static ppp_mppc_dec_t D;

/* ---- bit patterns as text ---- */
static char pat[80000];
static size_t pat_n;
static uint8_t vec[10000];

static void pat_clear(void) { pat_n = 0; pat[0] = 0; }
static void pat_add(const char *s)
{
    for (; *s; s++)
        if (*s == '0' || *s == '1') pat[pat_n++] = *s;
    pat[pat_n] = 0;
}
static void pat_num(unsigned v, int nbits)
{
    while (nbits-- > 0) pat[pat_n++] = (v >> nbits & 1) ? '1' : '0';
    pat[pat_n] = 0;
}
/* RFC 2118 4.1 */
static void p_lit(unsigned c)
{
    if (c < 0x80) { pat_add("0"); pat_num(c, 7); }
    else { pat_add("10"); pat_num(c & 0x7f, 7); }
}
/* 4.2.1 */
static void p_off(unsigned v)
{
    if (v < 64) { pat_add("1111"); pat_num(v, 6); }
    else if (v < 320) { pat_add("1110"); pat_num(v - 64, 8); }
    else { pat_add("110"); pat_num(v - 320, 13); }
}
/* 4.2.2 */
static void p_len(unsigned v)
{
    static const char *const pre[] = { "0", "10", "110", "1110", "11110", "111110", "1111110", "11111110",
                                       "111111110", "1111111110", "11111111110", "111111111110" };
    int k = 0;
    if (v == 3) { pat_add("0"); return; }
    while ((8u << k) <= v) k++;                 /* 4..7: k 0, 8..15: k 1, ... */
    pat_add(pre[k + 1]);
    pat_num(v & ((4u << k) - 1), k + 2);
}
/* the pattern as octets, most significant bit first, zero padded */
static size_t pat_bytes(void)
{
    size_t i, n = (pat_n + 7) / 8;
    memset(vec, 0, n);
    for (i = 0; i < pat_n; i++)
        if (pat[i] == '1') vec[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    return n;
}

static void test_rfc_examples(void)
{
    pat_clear(); p_lit(0x56); CHECK(strcmp(pat, "01010110") == 0);
    pat_clear(); p_lit(0xE7); CHECK(strcmp(pat, "101100111") == 0);
    pat_clear(); p_off(3);    CHECK(strcmp(pat, "1111000011") == 0);
    pat_clear(); p_off(128);  CHECK(strcmp(pat, "111001000000") == 0);
    pat_clear(); p_off(1024); CHECK(strcmp(pat, "1100001011000000") == 0);
    pat_clear(); p_len(15);   CHECK(strcmp(pat, "110111") == 0);
    pat_clear(); p_len(120);  CHECK(strcmp(pat, "111110111000") == 0);
    pat_clear(); p_len(4097); CHECK(strcmp(pat, "111111111110000000000001") == 0);
    pat_clear(); p_len(3);    CHECK(strcmp(pat, "0") == 0);
    pat_clear(); p_len(4);    CHECK(strcmp(pat, "1000") == 0);
    pat_clear(); p_len(7);    CHECK(strcmp(pat, "1011") == 0);
    pat_clear(); p_len(8191); CHECK(strcmp(pat, "111111111110111111111111") == 0);
}

static void test_decode_vectors(void)
{
    static uint8_t out[8192], want[8192];
    static const char sentence[] = "for whom the bell tolls, the bell tolls for thee.";
    static const unsigned offs[] = { 1, 2, 63, 64, 65, 319, 320, 321, 1024, 2000, 5000 };
    static const unsigned lens[] = { 3, 4, 5, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 511, 512, 1023, 1024,
                                     2047, 2048, 4095, 4096, 8189 };
    size_t n, i, k;
    int r;

    /* every literal */
    pat_clear();
    for (i = 0; i < 256; i++) p_lit((unsigned)i);
    n = pat_bytes();
    ppp_mppc_dec_init(&D);
    r = ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out);
    CHECK_EQ_I(r, 256);
    for (i = 0; i < 256; i++) CHECK_EQ_I(out[i], i);

    /* the example of RFC 2118 section 4 */
    pat_clear();
    for (i = 0; i < 24; i++) p_lit((uint8_t)sentence[i]);
    p_off(16); p_len(15);
    p_lit(' ');
    p_off(40); p_len(4);
    p_off(19); p_len(3);
    p_lit('e'); p_lit('.');
    n = pat_bytes();
    ppp_mppc_dec_init(&D);
    r = ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out);
    CHECK_EQ_I(r, 49);
    CHECK(r == 49 && memcmp(out, sentence, 49) == 0);

    /* every offset range, into an earlier packet: 5200 octets of history,
     * then copies of three octets from each distance */
    ppp_mppc_dec_init(&D);
    for (k = 0; k < 4; k++) {
        pat_clear();
        for (i = 0; i < 1300; i++) { want[k * 1300 + i] = (uint8_t)((k * 1300 + i) * 7 + (k * 1300 + i) / 251); p_lit(want[k * 1300 + i]); }
        n = pat_bytes();
        r = ppp_mppc_decompress(&D, k == 0, vec, n, out, sizeof out);
        CHECK_EQ_I(r, 1300);
        CHECK(memcmp(out, want + k * 1300, 1300) == 0);
    }
    pat_clear();
    k = 5200;
    for (i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        p_off(offs[i]); p_len(3);
        want[k] = want[k - offs[i]]; want[k + 1] = want[k + 1 - offs[i]]; want[k + 2] = want[k + 2 - offs[i]];
        k += 3;
    }
    n = pat_bytes();
    r = ppp_mppc_decompress(&D, 0, vec, n, out, sizeof out);
    CHECK_EQ_I(r, (long)(k - 5200));
    CHECK(r > 0 && memcmp(out, want + 5200, (size_t)r) == 0);
    /* the farthest copy that fits a history without wrapping */
    ppp_mppc_dec_init(&D);
    for (k = 0; k < 8189; ) {
        size_t m = 8189 - k < 1000 ? 8189 - k : 1000;
        pat_clear();
        for (i = 0; i < m; i++) { want[k + i] = (uint8_t)((k + i) * 13 + 5); p_lit(want[k + i]); }
        n = pat_bytes();
        CHECK_EQ_I(ppp_mppc_decompress(&D, k == 0, vec, n, out, sizeof out), (long)m);
        k += m;
    }
    pat_clear(); p_off(8189); p_len(3);
    n = pat_bytes();
    r = ppp_mppc_decompress(&D, 0, vec, n, out, sizeof out);
    CHECK_EQ_I(r, 3);
    CHECK(memcmp(out, want, 3) == 0);
    /* the history is full now: one more octet without the B bit is an error,
     * with it the packet starts at the front */
    pat_clear(); p_lit('z'); n = pat_bytes();
    CHECK_EQ_I(ppp_mppc_decompress(&D, 0, vec, n, out, sizeof out), -1);
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), 1);
    CHECK_EQ_I(out[0], 'z');

    /* every length range: "qr" and a copy from two back */
    for (i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        size_t j;
        pat_clear(); p_lit('q'); p_lit('r'); p_off(2); p_len(lens[i]);
        n = pat_bytes();
        ppp_mppc_dec_init(&D);
        r = ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out);
        CHECK_EQ_I(r, (long)lens[i] + 2);
        for (j = 0; r > 0 && j < (size_t)r; j++)
            if (out[j] != (j & 1 ? 'r' : 'q')) break;
        CHECK(r > 0 && j == (size_t)r);
    }

    /* what is not a packet */
    ppp_mppc_dec_init(&D);
    pat_clear(); p_lit('a'); pat_add("1111 000000"); p_len(3); n = pat_bytes();        /* offset 0 */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), -1);
    pat_clear(); p_lit('a'); p_lit('b'); p_off(2); pat_add("1111 1111 1111 0000 0000 0000 0000"); n = pat_bytes();  /* no such length */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), -1);
    pat_clear(); p_lit('a'); p_lit('b'); p_off(2); p_len(100); n = pat_bytes();
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, 101), -1);                      /* longer than the caller takes */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, 102), 102);
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n - 1, out, sizeof out), -1);           /* cut inside the length */
    pat_clear(); p_lit('a'); pat_add("10 1100"); n = pat_bytes();                      /* cut inside a literal */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), -1);
    pat_clear(); p_lit(0xE7); pat_add("101 1011"); n = pat_bytes();                    /* fewer than 8 bits left: padding, whatever it is */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), 1);
    CHECK_EQ_I(out[0], 0xE7);
    pat_clear(); p_lit('a'); p_lit('b'); pat_add("110 0000 0000"); n = pat_bytes();    /* cut inside an offset */
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, n, out, sizeof out), -1);
    CHECK_EQ_I(ppp_mppc_decompress(&D, 1, vec, 0, out, sizeof out), -1);               /* nothing at all */
}

/* ---- a second reader, strict: a copy may only reach back to the start of
 * the history's present lap (what the compressor here promises) ---- */
static uint8_t ref_hist[8192];
static size_t ref_hp;
static unsigned long ref_far;                   /* longest distance seen */

static int ref_bit(const uint8_t *in, size_t nbits, size_t *at)
{
    int b;
    if (*at >= nbits) return -1;
    b = in[*at / 8] >> (7 - *at % 8) & 1;
    (*at)++;
    return b;
}
static long ref_bits(const uint8_t *in, size_t nbits, size_t *at, int k)
{
    long v = 0;
    while (k-- > 0) { int b = ref_bit(in, nbits, at); if (b < 0) return -1; v = v << 1 | b; }
    return v;
}
static int ref_decode(unsigned bits, const uint8_t *in, size_t n, uint8_t *out)
{
    size_t at = 0, nbits = n * 8, start;
    if (bits & PPP_MPPC_FLUSHED) { memset(ref_hist, 0, sizeof ref_hist); ref_hp = 0; }
    if (bits & PPP_MPPC_AT_FRONT) ref_hp = 0;
    start = ref_hp;
    while (nbits - at >= 8) {
        long off, len;
        int ones = 0;
        if (ref_bit(in, nbits, &at) == 0) { ref_hist[ref_hp++] = (uint8_t)ref_bits(in, nbits, &at, 7); continue; }
        if (ref_bit(in, nbits, &at) == 0) { ref_hist[ref_hp++] = (uint8_t)(0x80 | ref_bits(in, nbits, &at, 7)); continue; }
        if (ref_bit(in, nbits, &at) == 0) off = ref_bits(in, nbits, &at, 13) + 320;
        else if (ref_bit(in, nbits, &at) == 0) off = ref_bits(in, nbits, &at, 8) + 64;
        else off = ref_bits(in, nbits, &at, 6);
        while (ref_bit(in, nbits, &at) == 1) ones++;
        if (ones == 0) len = 3;
        else len = (2L << ones) + ref_bits(in, nbits, &at, ones + 1);
        if (off <= 0 || (size_t)off > ref_hp || ref_hp + (size_t)len > 8192) return -1;
        if ((unsigned long)off > ref_far) ref_far = (unsigned long)off;
        while (len-- > 0) { ref_hist[ref_hp] = ref_hist[ref_hp - (size_t)off]; ref_hp++; }
    }
    memcpy(out, ref_hist + start, ref_hp - start);
    return (int)(ref_hp - start);
}

static unsigned long tot_in, tot_out, n_comp, n_exp, n_front, n_flushed;

static void one(const uint8_t *pkt, size_t n)
{
    static uint8_t c[PPP_MPPC_MAX_PKT + 8], o[PPP_MPPC_MAX_PKT + 8], o2[8192];
    static const uint8_t guard[4] = { 0xA5, 0x5A, 0xC3, 0x3C };
    size_t max = n - 1, cn;
    unsigned bits = 0xffff;

    memcpy(c + max, guard, 4);
    cn = ppp_mppc_compress(&E, pkt, 2, pkt + 2, n - 2, c, max, &bits);
    CHECK(cn <= max);
    CHECK(memcmp(c + max, guard, 4) == 0);
    CHECK((bits & ~(PPP_MPPC_FLUSHED | PPP_MPPC_AT_FRONT | PPP_MPPC_COMPRESSED)) == 0);
    tot_in += n;
    if (bits & PPP_MPPC_FLUSHED) n_flushed++;
    if (cn) {
        int r;
        CHECK(bits & PPP_MPPC_COMPRESSED);
        if (bits & PPP_MPPC_AT_FRONT) n_front++;
        if (bits & PPP_MPPC_FLUSHED) { CHECK(bits & PPP_MPPC_AT_FRONT); ppp_mppc_dec_flush(&D); }
        r = ppp_mppc_decompress(&D, (bits & PPP_MPPC_AT_FRONT) != 0, c, cn, o, PPP_MPPC_MAX_PKT);
        CHECK_EQ_I(r, (long)n);
        if (r == (int)n) CHECK(memcmp(o, pkt, n) == 0);
        r = ref_decode(bits, c, cn, o2);
        CHECK_EQ_I(r, (long)n);
        if (r == (int)n) CHECK(memcmp(o2, pkt, n) == 0);
        tot_out += cn; n_comp++;
    } else {
        /* expansion: the packet goes out as it is, and the next one must say FLUSHED */
        CHECK(!(bits & (PPP_MPPC_COMPRESSED | PPP_MPPC_AT_FRONT)));
        CHECK_EQ_I(E.flushed, 1);
        tot_out += n; n_exp++;
    }
}

static void sequence(int kind, int sizes, long count, uint32_t seed)
{
    static uint8_t pkt[PPP_MPPC_MAX_PKT];
    gen_t g = { seed };
    long i;

    ppp_mppc_enc_init(&E);
    ppp_mppc_dec_init(&D);
    ref_hp = 0;
    tot_in = tot_out = n_comp = n_exp = n_front = n_flushed = 0;
    for (i = 0; i < count; i++) {
        size_t n = sizes == 0 ? 1502 : sizes == 2 ? 3 + gen_n(&g, 40) : 42 + gen_n(&g, 1461);
        int k = kind < GEN_KINDS ? kind : (int)gen_n(&g, GEN_KINDS);
        pkt[0] = 0x00; pkt[1] = 0x21;
        if (n >= 42) gen_packet(&g, k, (uint32_t)i * 1460u, pkt + 2, n - 2);
        else gen_fill(&g, k, pkt + 2, n - 2);
        one(pkt, n);
    }
}

static void test_round_trips(int quick)
{
    static const char *const kn[] = { "HTML-like", "text", "binary", "random", "zeros", "mixed" };
    int k;

    for (k = 0; k <= GEN_KINDS; k++) {
        sequence(k, 0, quick ? 60 : 400, 0x1000u + (uint32_t)k);
        printf("  %-9s full packets: %lu -> %lu octets, %lu compressed, %lu expanded, %lu at the front\n",
               kn[k], tot_in, tot_out, n_comp, n_exp, n_front);
        if (k == GEN_HTML || k == GEN_TEXT) CHECK(tot_out * 3 < tot_in * 2);
        if (k == GEN_ZERO) CHECK(tot_out * 20 < tot_in);
        if (k == GEN_RANDOM) { CHECK_EQ_I(n_comp, 0); CHECK_EQ_I(n_flushed, quick ? 60 : 400); }
        /* five full packets fill the history: the sixth starts at the front */
        if (k == GEN_HTML) { CHECK(n_front >= (quick ? 60u : 400u) / 6); CHECK_EQ_I(n_flushed, 1); }
    }
    sequence(GEN_KINDS, 1, quick ? 5000 : 120000, 99);
    printf("  %ld packets, mixed kinds and sizes: %lu -> %lu octets (%lu expanded, %lu flushed, %lu at the front); farthest copy %lu\n",
           quick ? 5000L : 120000L, tot_in, tot_out, n_exp, n_flushed, n_front, ref_far);
    CHECK(tot_out < tot_in);
    CHECK(n_exp > 0 && n_front > (quick ? 100u : 3000u));
    CHECK(ref_far > 6000 && ref_far < 8192);
    sequence(GEN_TEXT, 2, quick ? 2000 : 20000, 5);
    CHECK(tot_out < tot_in);
}

/* the compressor's exact output for small inputs, bit for bit */
static void test_encode_exact(void)
{
    static uint8_t c[64];
    unsigned bits;
    size_t n, cn;

    ppp_mppc_enc_init(&E);
    cn = ppp_mppc_compress(&E, (const uint8_t *)"abc", 3, (const uint8_t *)"abcabc", 6, c, sizeof c, &bits);
    pat_clear(); p_lit('a'); p_lit('b'); p_lit('c'); p_off(3); p_len(6);
    n = pat_bytes();
    CHECK_EQ_I(cn, (long)n);
    CHECK(cn == n && memcmp(c, vec, n) == 0);
    CHECK_EQ_I(bits, PPP_MPPC_FLUSHED | PPP_MPPC_AT_FRONT | PPP_MPPC_COMPRESSED);
    /* the next packet finds the first in the history: one copy, from the
     * nearest place (three back, running into itself) */
    cn = ppp_mppc_compress(&E, (const uint8_t *)"abcabcabc", 9, NULL, 0, c, sizeof c, &bits);
    pat_clear(); p_off(3); p_len(9);
    n = pat_bytes();
    CHECK_EQ_I(cn, (long)n);
    CHECK(cn == n && memcmp(c, vec, n) == 0);
    CHECK_EQ_I(bits, PPP_MPPC_COMPRESSED);
    /* literals above 0x7F take nine bits */
    ppp_mppc_enc_flush(&E);
    cn = ppp_mppc_compress(&E, (const uint8_t *)"\xE7\x56", 2, NULL, 0, c, sizeof c, &bits);
    pat_clear(); p_lit(0xE7); p_lit(0x56);
    n = pat_bytes();
    CHECK_EQ_I(cn, 3);
    CHECK(cn == n && memcmp(c, vec, n) == 0);
    CHECK_EQ_I(bits, PPP_MPPC_FLUSHED | PPP_MPPC_AT_FRONT | PPP_MPPC_COMPRESSED);
    /* and with the room a caller gives (no longer than the packet) that is expansion */
    ppp_mppc_enc_flush(&E);
    cn = ppp_mppc_compress(&E, (const uint8_t *)"\xE7\x56", 2, NULL, 0, c, 2, &bits);
    CHECK_EQ_I(cn, 0);
    CHECK_EQ_I(bits, PPP_MPPC_FLUSHED);
    cn = ppp_mppc_compress(&E, (const uint8_t *)"aaaaaaaa", 8, NULL, 0, c, 8, &bits);
    CHECK(cn > 0 && cn < 8);
    CHECK_EQ_I(bits, PPP_MPPC_FLUSHED | PPP_MPPC_AT_FRONT | PPP_MPPC_COMPRESSED);
}

/* the history wraps: the B bit, and nothing is copied across the wrap */
static void test_wrap(void)
{
    static uint8_t pkt[1502], c[1502];
    unsigned bits;
    size_t cn, cn2;
    int i, fronts = 0;
    gen_t g = { 3 };

    ppp_mppc_enc_init(&E);
    pkt[0] = 0; pkt[1] = 0x21;
    gen_fill(&g, GEN_TEXT, pkt + 2, 1500);
    cn = ppp_mppc_compress(&E, pkt, 2, pkt + 2, 1500, c, 1501, &bits);
    CHECK(cn > 0);
    for (i = 1; i < 12; i++) {
        /* the same packet again: one long copy while the history has it */
        cn2 = ppp_mppc_compress(&E, pkt, 2, pkt + 2, 1500, c, 1501, &bits);
        if (bits & PPP_MPPC_AT_FRONT) {
            /* 5 x 1502 = 7510: the sixth packet does not fit; at the front
             * it has nothing before it and costs what the first one did */
            fronts++;
            CHECK_EQ_I(i % 5, 0);
            CHECK_EQ_I(cn2, (long)cn);
        } else {
            CHECK(cn2 > 0 && cn2 <= 6);
        }
        CHECK(!(bits & PPP_MPPC_FLUSHED));
    }
    CHECK_EQ_I(fronts, 2);
}

static void test_garbage(int quick)
{
    static uint8_t in[1600], out[8192 + 8];
    static const uint8_t guard[4] = { 1, 2, 3, 4 };
    gen_t g = { 0xBADF00D };
    long i, errors = 0, rounds = quick ? 4000 : 60000;

    ppp_mppc_dec_init(&D);
    for (i = 0; i < rounds; i++) {
        size_t n = gen_n(&g, i % 7 == 0 ? 1500 : 60), k;
        size_t max = i % 5 == 0 ? gen_n(&g, 200) : i % 5 == 1 ? 8192 : PPP_MPPC_MAX_PKT;
        int r;
        for (k = 0; k < n; k++) in[k] = (uint8_t)(gen_u32(&g) >> 9);
        memcpy(out + max, guard, 4);
        r = ppp_mppc_decompress(&D, (int)(i & 1), in, n, out, max);
        CHECK(r == -1 || (r >= 0 && (size_t)r <= max));
        CHECK(memcmp(out + max, guard, 4) == 0);
        CHECK(D.hp <= PPP_MPPC_HIST);
        if (r < 0) errors++;
    }
    printf("  %ld garbage packets: %ld rejected\n", rounds, errors);
    CHECK(errors > 0);
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && strcmp(argv[1], "quick") == 0;

    test_rfc_examples();
    test_decode_vectors();
    test_encode_exact();
    test_wrap();
    test_round_trips(quick);
    test_garbage(quick);
    printf("  RAM: compressor %lu octets, decompressor %lu\n", (unsigned long)sizeof E, (unsigned long)sizeof D);
    T_REPORT("test_mppc");
}
