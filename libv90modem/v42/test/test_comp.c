/* V.42 bis tests. Bit-exact streams derived by hand from the Recommendation
 * (clauses 6, 7, 9, Table 2, Figure 3, Appendix II.4), decoder error
 * indications (5.8), and round trips over varied data with the dictionary
 * filling and recycling, random buffer sizes and flush points. */
#include <string.h>
#include "test.h"
#include "v42_comp.h"

static unsigned rnd_state = 777;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7FFF;
}

static v42comp_enc_t enc;
static v42comp_dec_t dec;

/* ---- an independent bit packer for building expected streams (7.5) ---- */
typedef struct { uint8_t b[256]; int n; uint32_t acc; int nb; } packer;
static void pk_init(packer *p) { memset(p, 0, sizeof *p); }
static void pk_octet(packer *p, int o) { p->b[p->n++] = (uint8_t)o; }
static void pk_code(packer *p, unsigned code, int bits)
{
    p->acc |= (uint32_t)code << p->nb; p->nb += bits;
    while (p->nb >= 8) { p->b[p->n++] = (uint8_t)p->acc; p->acc >>= 8; p->nb -= 8; }
}
static void pk_align(packer *p)
{
    if (p->nb) { p->b[p->n++] = (uint8_t)p->acc; p->acc = 0; p->nb = 0; }
}

#define CW(c) ((unsigned)(c) + 3)      /* codeword of a root node (6.2) */
enum { ETM = 0, FLUSH = 1, STEPUP = 2 };       /* Table 2 control codewords */
enum { ECM = 0, EID = 1, RESET = 2 };          /* Table 2 command codes */

static size_t enc_all(const void *in, size_t n, uint8_t *out, size_t max, int flush)
{
    size_t outlen = 0, c;
    c = v42comp_encode(&enc, (const uint8_t *)in, n, out, max, &outlen);
    CHECK_EQ_I(c, n);
    if (flush) CHECK_EQ_I(v42comp_flush(&enc, out, max, &outlen), 1);
    return outlen;
}

static void test_transparent(void)
{
    uint8_t out[64]; size_t n;
    static const uint8_t z[1] = { 0 };
    static const uint8_t e2[3] = { 51, 0, 103 };

    /* initial state is transparent mode: characters pass octet aligned */
    CHECK_EQ_I(v42comp_enc_init(&enc, 512, 6), 0);
    v42comp_enc_policy(&enc, V42COMP_NEVER);
    CHECK_EQ_I(enc.compressed, 0);
    CHECK_EQ_I(enc.c2, 9);
    CHECK_EQ_I(enc.c3, 512);
    CHECK_EQ_I(enc.d.c1, 259);
    CHECK_EQ_I(enc.esc, 0);
    n = enc_all("ABC", 3, out, sizeof out, 1);
    CHECK_EQ_I(n, 3);
    CHECK(memcmp(out, "ABC", 3) == 0);

    /* 9.2: the escape character (initially 0) is sent followed by EID, and
     * then cycles by 51 modulo 256 */
    n = enc_all(z, 1, out, sizeof out, 1);
    CHECK_EQ_I(n, 2);
    CHECK_EQ_I(out[0], 0); CHECK_EQ_I(out[1], EID);
    CHECK_EQ_I(enc.esc, 51);
    /* now 51 is the escape and 0 is ordinary; 102 is next in line */
    n = enc_all(e2, 3, out, sizeof out, 1);
    CHECK_EQ_I(n, 4);
    CHECK_EQ_I(out[0], 51); CHECK_EQ_I(out[1], EID);
    CHECK_EQ_I(out[2], 0);
    CHECK_EQ_I(out[3], 103);
    CHECK_EQ_I(enc.esc, 102);

    /* transparent mode still builds the dictionary (II.4.2): "AB" "BC" exist */
    CHECK_EQ_I(enc.d.parent[259], CW('A')); CHECK_EQ_I(enc.d.ch[259], 'B');
    CHECK_EQ_I(enc.d.parent[260], CW('B')); CHECK_EQ_I(enc.d.ch[260], 'C');

    /* parameter range checks (5.1) */
    CHECK_EQ_I(v42comp_enc_init(&enc, 511, 6), -1);
    CHECK_EQ_I(v42comp_enc_init(&enc, 512, 5), -1);
    CHECK_EQ_I(v42comp_enc_init(&enc, 512, 251), -1);
    CHECK_EQ_I(v42comp_enc_init(&enc, V42COMP_MAX_CODEWORDS + 1, 6), -1);
    CHECK_EQ_I(v42comp_dec_init(&dec, 511, 6), -1);
    CHECK_EQ_I(v42comp_enc_init(&enc, V42COMP_MAX_CODEWORDS, 250), 0);
}

static void test_compressed_vectors(void)
{
    uint8_t out[64]; size_t n;
    packer p;

    /* enter compressed mode at once: escape, ECM; then "A", flushed:
     * codeword 68 in 9 bits, FLUSH, zero bits to the octet boundary */
    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    n = enc_all("A", 1, out, sizeof out, 1);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, FLUSH, 9); pk_align(&p);
    CHECK_EQ_I(n, p.n);
    CHECK(memcmp(out, p.b, (size_t)p.n) == 0);
    CHECK_EQ_I(n, 5);
    CHECK_EQ_I(out[2], 0x44); CHECK_EQ_I(out[3], 0x02); CHECK_EQ_I(out[4], 0x00);

    /* Appendix II.4.3, "CCCCC": C, C, CC, then the pending C on flush. The
     * second C must not use the entry "CC" made just before it. */
    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    n = enc_all("CCCCC", 5, out, sizeof out, 1);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('C'), 9); pk_code(&p, CW('C'), 9);
    pk_code(&p, 259, 9); pk_code(&p, CW('C'), 9);
    pk_code(&p, FLUSH, 9); pk_align(&p);
    CHECK_EQ_I(n, p.n);
    CHECK(memcmp(out, p.b, (size_t)p.n) == 0);
    CHECK_EQ_I(enc.d.parent[259], CW('C'));          /* "CC"  */
    CHECK_EQ_I(enc.d.parent[260], 259);              /* "CCC" */

    /* Appendix II.4.1, "BAY" with "BA" known: after "BABAY" the stream is
     * B, A, BA (259), and Y on flush; "BAY" becomes an entry */
    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    n = enc_all("BABAY", 5, out, sizeof out, 1);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('B'), 9); pk_code(&p, CW('A'), 9);
    pk_code(&p, 259, 9); pk_code(&p, CW('Y'), 9);
    pk_code(&p, FLUSH, 9); pk_align(&p);
    CHECK_EQ_I(n, p.n);
    CHECK(memcmp(out, p.b, (size_t)p.n) == 0);
    CHECK_EQ_I(enc.d.parent[261], 259); CHECK_EQ_I(enc.d.ch[261], 'Y');

    /* a flush that lands on an octet boundary sends no FLUSH codeword
     * (7.9 c): eight 9-bit codewords are exactly nine octets */
    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    n = enc_all("abcdefgh", 8, out, sizeof out, 1);
    CHECK_EQ_I(n, 2 + 9);
    /* a second flush with nothing new sends nothing */
    n = 0;
    CHECK_EQ_I(v42comp_flush(&enc, out, sizeof out, &n), 1);
    CHECK_EQ_I(n, 0);

    /* the escape character is not escaped in compressed mode but still
     * cycles (9.2 b) */
    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    out[0] = 0; out[1] = 0;
    {
        static const uint8_t zz[2] = { 0, 51 };
        uint8_t o2[32];
        n = enc_all(zz, 2, o2, sizeof o2, 1);
        pk_init(&p);
        pk_octet(&p, 0); pk_octet(&p, ECM);
        pk_code(&p, CW(0), 9); pk_code(&p, CW(51), 9);
        pk_code(&p, FLUSH, 9); pk_align(&p);
        CHECK_EQ_I(n, p.n);
        CHECK(memcmp(o2, p.b, (size_t)p.n) == 0);
        CHECK_EQ_I(enc.esc, 102);
    }
}

static int dec_all(const uint8_t *in, size_t n, uint8_t *out, size_t max, size_t *outlen)
{
    size_t used = 0;
    int r = v42comp_decode(&dec, in, n, &used, out, max, outlen);
    if (r == 0) CHECK_EQ_I(used, n);
    return r;
}

static void test_decoder_vectors(void)
{
    packer p; uint8_t out[600]; size_t n;

    /* STEPUP (7.4): codeword size goes from 9 to 10 bits */
    v42comp_dec_init(&dec, 1024, 6);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, STEPUP, 9);
    pk_code(&p, CW('B'), 10); pk_code(&p, FLUSH, 10); pk_align(&p);
    n = 0;
    CHECK_EQ_I(dec_all(p.b, (size_t)p.n, out, sizeof out, &n), 0);
    CHECK_EQ_I(n, 2);
    CHECK(memcmp(out, "AB", 2) == 0);
    CHECK_EQ_I(dec.c2, 10);
    CHECK_EQ_I(dec.d.parent[259], CW('A')); CHECK_EQ_I(dec.d.ch[259], 'B');

    /* 5.8 a): STEPUP taking C2 beyond N1 (N2 = 512 gives N1 = 9) */
    v42comp_dec_init(&dec, 512, 6);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, STEPUP, 9); pk_align(&p);
    n = 0;
    CHECK_EQ_I(dec_all(p.b, (size_t)p.n, out, sizeof out, &n), V42COMP_ERR_STEPUP);
    /* the error is sticky */
    CHECK_EQ_I(dec_all((const uint8_t *)"x", 1, out, sizeof out, &n), V42COMP_ERR_STEPUP);

    /* 5.8 b): codeword equal to C1 */
    v42comp_dec_init(&dec, 512, 6);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, 259, 9); pk_align(&p);
    n = 0;
    CHECK_EQ_I(dec_all(p.b, (size_t)p.n, out, sizeof out, &n), V42COMP_ERR_C1);

    /* 5.8 c): codeword of an empty dictionary entry */
    v42comp_dec_init(&dec, 512, 6);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, 300, 9); pk_align(&p);
    n = 0;
    CHECK_EQ_I(dec_all(p.b, (size_t)p.n, out, sizeof out, &n), V42COMP_ERR_EMPTY);

    /* 5.8 d): reserved command code */
    v42comp_dec_init(&dec, 512, 6);
    out[0] = 'A'; out[1] = 0; out[2] = 3;
    {
        uint8_t in[3] = { 'A', 0, 3 }, o[8];
        n = 0;
        CHECK_EQ_I(dec_all(in, 3, o, sizeof o, &n), V42COMP_ERR_COMMAND);
        CHECK_EQ_I(n, 1);
    }

    /* Figure 3: ETM is followed by zero bits to the octet boundary and then
     * transparent characters; "A", "B" compressed then "C" transparent */
    v42comp_dec_init(&dec, 2048, 6);
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('A'), 9); pk_code(&p, STEPUP, 9); pk_code(&p, STEPUP, 10);
    pk_code(&p, CW('B'), 11); pk_code(&p, ETM, 11);
    CHECK_EQ_I(p.nb, 2);                 /* six zero bits of padding follow */
    pk_align(&p);
    pk_octet(&p, 'C'); pk_octet(&p, 'D');
    n = 0;
    CHECK_EQ_I(dec_all(p.b, (size_t)p.n, out, sizeof out, &n), 0);
    CHECK_EQ_I(n, 4);
    CHECK(memcmp(out, "ABCD", 4) == 0);
    CHECK_EQ_I(dec.compressed, 0);
    /* dictionary: "AB" from the codewords, "BC" from the mode change
     * (7.8.2 b), "CD" from transparent matching */
    CHECK_EQ_I(dec.d.parent[259], CW('A')); CHECK_EQ_I(dec.d.ch[259], 'B');
    CHECK_EQ_I(dec.d.parent[260], CW('B')); CHECK_EQ_I(dec.d.ch[260], 'C');
    CHECK_EQ_I(dec.d.parent[261], CW('C')); CHECK_EQ_I(dec.d.ch[261], 'D');

    /* RESET command code (7.8.3, clause 8): dictionary and escape start over */
    v42comp_dec_init(&dec, 512, 6);
    {
        uint8_t in[] = { 'A', 'B', 0, EID, 51, RESET, 'X', 0, EID };
        n = 0;
        CHECK_EQ_I(dec_all(in, sizeof in, out, sizeof out, &n), 0);
        CHECK_EQ_I(n, 5);
        CHECK(memcmp(out, "AB\0X\0", 5) == 0);
        CHECK_EQ_I(dec.resets, 1);
        CHECK_EQ_I(dec.esc, 51);
        CHECK_EQ_I(dec.d.parent[259], CW('X'));      /* "X\0", not "AB" */
    }

    /* bounded output: a codeword whose string does not fit is held back */
    v42comp_enc_init(&enc, 512, 32);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    v42comp_dec_init(&dec, 512, 32);
    {
        uint8_t src[200], cod[300], o[200]; size_t cl, used, ol, total = 0, off = 0;
        int guard = 0;
        memset(src, 'r', sizeof src);
        cl = enc_all(src, sizeof src, cod, sizeof cod, 1);
        CHECK(cl < 60);
        while ((off < cl || total < sizeof src) && guard++ < 10000) {
            ol = 0;
            /* only 3 octets of room at first: long strings cannot come out */
            CHECK_EQ_I(v42comp_decode(&dec, cod + off, cl - off, &used,
                                      o + total, guard < 50 ? 3 : sizeof o - total, &ol), 0);
            off += used; total += ol;
        }
        CHECK_EQ_I(total, sizeof src);
        CHECK(memcmp(o, src, sizeof src) == 0);
    }
}

/* ------------------------------------------------------------ round trips */
#define BIG 120000
static uint8_t src[BIG], cod[2 * BIG + 1000], back[BIG + 300];

static int dict_equal(void)
{
    int i;
    if (enc.d.c1 != dec.d.c1) return 0;
    for (i = V42COMP_N5; i < enc.d.n2; i++) {
        if (enc.d.parent[i] != dec.d.parent[i]) return 0;
        if (enc.d.parent[i] && enc.d.ch[i] != dec.d.ch[i]) return 0;
    }
    return enc.esc == dec.esc && enc.compressed == dec.compressed && enc.c2 == dec.c2;
}

/* Encode src[0..n) in random sized pieces with random output room and
 * random flushes, decoding as we go; returns the compressed size or 0 on a
 * mismatch. After every flush the two dictionaries must be identical. */
static size_t roundtrip(size_t n, int n2, int n7, int policy, int flush_pct)
{
    size_t in = 0, total = 0, got = 0;
    int dict_ok = 1, err = 0;

    CHECK_EQ_I(v42comp_enc_init(&enc, n2, n7), 0);
    CHECK_EQ_I(v42comp_dec_init(&dec, n2, n7), 0);
    v42comp_enc_policy(&enc, policy);
    while (in < n || v42comp_enc_pending(&enc)) {
        uint8_t piece[300];
        size_t want = 1 + rnd() % 200, lim = rnd() % 4 ? 300 : 9, room = 1 + rnd() % lim;
        size_t outlen = 0, c, used, off = 0;
        int flushed = 0;
        if (want > n - in) want = n - in;
        c = v42comp_encode(&enc, src + in, want, piece, room, &outlen);
        in += c;
        if (c == want && ((int)(rnd() % 100) < flush_pct || in == n))
            flushed = v42comp_flush(&enc, piece, room, &outlen);
        if (total + outlen <= sizeof cod) memcpy(cod + total, piece, outlen);
        total += outlen;
        while (off < outlen || (dec.compressed && dec.nbits >= dec.c2)) {
            size_t ol = 0;
            size_t before = off;
            if (v42comp_decode(&dec, piece + off, outlen - off, &used,
                               back + got, sizeof back - got, &ol) != 0) { err = 1; break; }
            off += used; got += ol;
            if (off == before && ol == 0) break;
        }
        if (err) break;
        if (flushed && !dict_equal()) dict_ok = 0;
    }
    CHECK_EQ_I(err, 0);
    CHECK(dict_ok);
    CHECK_EQ_I(got, n);
    if (got != n || memcmp(src, back, n) != 0) { CHECK(memcmp(src, back, n) == 0); return 0; }
    CHECK_EQ_I(enc.in_chars, n);
    CHECK_EQ_I(enc.out_octets, total);
    return total;
}


/* 6.5 node recovery, checked against a hand-built dictionary: when the last
 * entry is assigned, C1 wraps to N5, skips entries that are in use and not
 * leaves, and detaches the first leaf it meets. */
static void test_recovery_vector(void)
{
    uint8_t in[300], out[700]; size_t n = 0, k;

    v42comp_enc_init(&enc, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_NEVER);
    /* "abxabc": ab=259, bx=260, xa=261, abc=262 (so 259 is not a leaf) */
    memcpy(in, "abxabc", 6); n = 6;
    /* 249 more characters, each making one new two character entry */
    for (k = 128; k <= 255; k++) in[n++] = (uint8_t)k;
    for (k = 0; k <= 96; k++) in[n++] = (uint8_t)k;
    for (k = 127; k >= 104; k--) in[n++] = (uint8_t)k;
    CHECK_EQ_I(n, 6 + 249);
    enc_all(in, n - 1, out, sizeof out, 0);
    /* one short of full: 511 is still the next empty entry */
    CHECK_EQ_I(enc.d.c1, 511);
    CHECK_EQ_I(enc.d.recovered, 0);
    CHECK_EQ_I(enc.d.parent[260], CW('b'));
    enc_all(in + n - 1, 1, out, sizeof out, 0);
    CHECK_EQ_I(enc.d.added, 253);
    CHECK_EQ_I(enc.d.parent[511], CW(105)); CHECK_EQ_I(enc.d.ch[511], 104);
    CHECK_EQ_I(enc.d.parent[259], CW('a'));          /* "ab" kept: has "abc" */
    CHECK_EQ_I(enc.d.parent[262], 259);
    CHECK_EQ_I(enc.d.parent[260], 0);                /* "bx" was the first leaf */
    CHECK_EQ_I(enc.d.c1, 260);
    CHECK_EQ_I(enc.d.recovered, 1);
    /* the next new string takes entry 260 and 261 ("xa", a leaf) is freed */
    in[0] = 200;
    enc_all(in, 1, out, sizeof out, 0);
    CHECK_EQ_I(enc.d.parent[260], CW(104)); CHECK_EQ_I(enc.d.ch[260], 200);
    CHECK_EQ_I(enc.d.parent[261], 0);
    CHECK_EQ_I(enc.d.c1, 261);
}

/* 7.8.2: on leaving compressed mode the pending string is sent, and the
 * first transparent character only updates the dictionary and restarts the
 * match, even when string plus character is already an entry. Encoder
 * output is checked bit for bit, and the decoder must build the same tree. */
static void test_etm_vector(void)
{
    uint8_t out[64], o[64]; size_t n, m, ol = 0;
    packer p;

    v42comp_enc_init(&enc, 512, 6);
    v42comp_dec_init(&dec, 512, 6);
    v42comp_enc_policy(&enc, V42COMP_ALWAYS);
    n = enc_all("ababa", 5, out, sizeof out, 0);     /* a, b, "ab"; "a" pending */
    v42comp_enc_policy(&enc, V42COMP_NEVER);
    m = 0;
    CHECK_EQ_I(v42comp_encode(&enc, (const uint8_t *)"bab", 3, out + n, sizeof out - n, &m), 3);
    n += m;
    pk_init(&p);
    pk_octet(&p, 0); pk_octet(&p, ECM);
    pk_code(&p, CW('a'), 9); pk_code(&p, CW('b'), 9); pk_code(&p, 259, 9);
    pk_code(&p, CW('a'), 9); pk_code(&p, ETM, 9); pk_align(&p);
    pk_octet(&p, 'b'); pk_octet(&p, 'a'); pk_octet(&p, 'b');
    CHECK_EQ_I(n, p.n);
    CHECK(memcmp(out, p.b, (size_t)p.n) == 0);
    /* "a"+"b" exists (259) so nothing was added for it; matching restarted
     * at "b", went on to "ba" (260), and "bab" became entry 262 */
    CHECK_EQ_I(enc.d.parent[261], 259); CHECK_EQ_I(enc.d.ch[261], 'a');
    CHECK_EQ_I(enc.d.parent[262], 260); CHECK_EQ_I(enc.d.ch[262], 'b');
    CHECK_EQ_I(dec_all(out, n, o, sizeof o, &ol), 0);
    CHECK_EQ_I(ol, 8);
    CHECK(memcmp(o, "abababab", 8) == 0);
    CHECK_EQ_I(dec.d.parent[262], 260); CHECK_EQ_I(dec.d.ch[262], 'b');
    CHECK(dict_equal());
}

static const char *words[] = {
    "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "and ",
    "PPP ", "frame ", "modem ", "V.42bis ", "dictionary ", "codeword ", "string ",
    "\r\n", "compression ", "transparent ", "of ", "to ", "in ", "is ", "error "
};

static size_t fill_text(size_t n)
{
    size_t i = 0;
    while (i < n) {
        const char *w = words[rnd() % (sizeof words / sizeof words[0])];
        while (*w && i < n) src[i++] = (uint8_t)*w++;
    }
    return n;
}

static void fill_random(size_t off, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) src[off + i] = (uint8_t)(rnd() >> 3);
}

static void test_roundtrips(void)
{
    static const int n2s[] = { 512, 1024, V42COMP_MAX_CODEWORDS };
    static const int n7s[] = { 6, 32, 250 };
    size_t out, i, n;
    int a, b, pol;

    /* text: compresses well in every configuration, and the dictionary
     * fills up and recycles entries */
    for (a = 0; a < 3; a++) for (b = 0; b < 3; b++) for (pol = 0; pol < 3; pol++) {
        n = fill_text(60000);
        out = roundtrip(n, n2s[a], n7s[b], pol, 3);
        CHECK(out > 0);
        if (pol == V42COMP_NEVER) CHECK(out == n);     /* no zero octets in text */
        else CHECK(out * 100 < n * 62);
        if (pol == V42COMP_ALWAYS) CHECK(enc.compressed);
        if (pol == V42COMP_NEVER) CHECK(!enc.compressed);
        CHECK(enc.d.added > (unsigned long)(n2s[a] - V42COMP_N5));
        CHECK(enc.d.recovered > 100);
        CHECK(enc.d.recovered == dec.d.recovered);
    }
    /* long strings really are used when allowed */
    n = fill_text(60000);
    out = roundtrip(n, V42COMP_MAX_CODEWORDS, 250, V42COMP_ALWAYS, 0);
    i = out;
    out = roundtrip(n, V42COMP_MAX_CODEWORDS, 6, V42COMP_ALWAYS, 0);
    CHECK(i < out);

    /* random octets (already compressed looking): the mode test keeps the
     * encoder transparent, cost limited to escapes; forced compressed mode
     * still round trips though it expands */
    fill_random(0, 60000);
    out = roundtrip(60000, 2048 <= V42COMP_MAX_CODEWORDS ? 2048 : 512, 32, V42COMP_AUTO, 2);
    CHECK(out > 0 && out < 60000 + 60000 / 50);
    out = roundtrip(60000, 512, 6, V42COMP_ALWAYS, 2);
    CHECK(out > 60000);
    CHECK(enc.c2 == 9);
    out = roundtrip(60000, 1024, 250, V42COMP_ALWAYS, 0);
    CHECK(out > 60000);
    CHECK(enc.c2 == 10);                 /* STEPUP happened */

    /* long runs of one character, and runs of several */
    memset(src, 'x', BIG);
    out = roundtrip(BIG, 1024, 250, V42COMP_AUTO, 0);
    CHECK(out > 0 && out * 50 < BIG);
    out = roundtrip(BIG, 512, 6, V42COMP_AUTO, 1);
    CHECK(out > 0 && out * 4 < BIG);
    for (i = 0; i < BIG; i++) src[i] = (uint8_t)("aaaaaaaaaaaaaaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbc"[i % 55]);
    out = roundtrip(BIG, 1024, 32, V42COMP_AUTO, 5);
    CHECK(out > 0 && out * 8 < BIG);
    memset(src, 0, BIG);                 /* zero is the first escape */
    out = roundtrip(BIG, 1024, 250, V42COMP_AUTO, 1);
    CHECK(out > 0 && out * 20 < BIG);

    /* the escape character itself: every octet is the current escape, in
     * all three policies; transparent mode doubles the size exactly */
    for (i = 0; i < 30000; i++) src[i] = (uint8_t)(51 * i);
    out = roundtrip(30000, 512, 6, V42COMP_NEVER, 5);
    CHECK_EQ_I(out, 60000);
    out = roundtrip(30000, 512, 6, V42COMP_ALWAYS, 5);
    CHECK(out > 0);
    out = roundtrip(30000, 1024, 32, V42COMP_AUTO, 5);
    CHECK(out > 0 && out < 30000);       /* period 256 pattern compresses */
    /* escapes scattered through random data and through text */
    fill_random(0, 40000);
    for (i = 0; i < 40000; i += 3) src[i] = (uint8_t)(rnd() % 5 * 51);
    out = roundtrip(40000, 1024, 32, V42COMP_AUTO, 5);
    CHECK(out > 0);
    out = roundtrip(40000, 1024, 32, V42COMP_NEVER, 5);
    CHECK(out > 40000);

    /* alternating compressible and incompressible stretches: the encoder
     * changes mode in both directions (7.8.1, 7.8.2) */
    fill_text(BIG);
    for (i = 0; i + 8000 <= BIG; i += 16000) fill_random(i, 8000);
    out = roundtrip(BIG, 1024, 32, V42COMP_AUTO, 2);
    CHECK(out > 0 && out < BIG * 85 / 100);
    CHECK(enc.mode_switches >= 8);

    /* every flush pattern from none to after every piece, tiny inputs */
    for (a = 0; a <= 100; a += 20) {
        n = fill_text(5000);
        CHECK(roundtrip(n, 512, 6, V42COMP_AUTO, a) > 0);
        CHECK(roundtrip(n, 512, 250, V42COMP_ALWAYS, a) > 0);
    }
    /* binary structure: a counter pattern with repeats */
    for (i = 0; i < BIG; i++) src[i] = (uint8_t)((i >> 2) ^ (i * 7));
    CHECK(roundtrip(BIG, V42COMP_MAX_CODEWORDS, 250, V42COMP_AUTO, 1) > 0);
    /* nothing at all */
    CHECK_EQ_I(roundtrip(0, 512, 6, V42COMP_AUTO, 100), 0);
}

int main(void)
{
    test_transparent();
    test_compressed_vectors();
    test_decoder_vectors();
    test_recovery_vector();
    test_etm_vector();
    test_roundtrips();
    T_REPORT("test_comp");
}
