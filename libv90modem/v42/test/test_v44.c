/* V.44 tests. Bit-exact streams from the Recommendation (Table 6 of 6.6.4,
 * the two worked examples of Appendix II) and derived by hand from clauses
 * 6 and 7; decoder C-ERROR conditions (7.15) and garbage input; round trips
 * over varied data, parameters, buffer sizes and flush points; and, printed
 * only, compression ratios next to V.42 bis and the processing speed. */
#include <string.h>
#include <time.h>
#include "test.h"
#include "v44_comp.h"
#include "v42_comp.h"

static unsigned rnd_state = 4444;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7FFF;
}

static v44comp_enc_t enc;
static v44comp_dec_t dec;

/* ---- an independent bit packer for building expected streams (6.6) ---- */
typedef struct { uint8_t b[4096]; int n; uint32_t acc; int nb; } packer;
static void pk_init(packer *p) { memset(p, 0, sizeof *p); }
static void pk_bits(packer *p, unsigned v, int bits)
{
    p->acc |= (uint32_t)v << p->nb; p->nb += bits;
    while (p->nb >= 8) { p->b[p->n++] = (uint8_t)p->acc; p->acc >>= 8; p->nb -= 8; }
}
static void pk_align(packer *p)
{
    if (p->nb) { p->b[p->n++] = (uint8_t)p->acc; p->acc = 0; p->nb = 0; }
}
static void pk_octet(packer *p, int o) { p->b[p->n++] = (uint8_t)o; }
/* Table 5: prefixes. after_cw = the code before this one was a codeword */
static void pk_ord(packer *p, int ch, int c5, int after_cw)
{
    pk_bits(p, 0, after_cw ? 2 : 1);
    pk_bits(p, (unsigned)ch, c5);
}
static void pk_cw(packer *p, unsigned cw, int c2) { pk_bits(p, 1, 1); pk_bits(p, cw, c2); }
/* Tables 3 and 4, bit by bit in the order of transmission given there */
static void pk_ext(packer *p, int len, int n7)
{
    static const char *t3[13] = { "", "1", "010", "001", "011", "0000000", "0000100", "0000010",
                                  "0000110", "0000001", "0000101", "0000011", "0000111" };
    const char *s;
    pk_bits(p, 0, 1); pk_bits(p, 1, 1);         /* prefix "0" "1" */
    if (len <= 12) {
        for (s = t3[len]; *s; s++) pk_bits(p, (unsigned)(*s - '0'), 1);
    } else {
        int nb = n7 <= 46 ? 5 : n7 <= 78 ? 6 : n7 <= 142 ? 7 : 8, i, v = len - 13;
        pk_bits(p, 0, 1); pk_bits(p, 0, 1); pk_bits(p, 0, 1); pk_bits(p, 1, 1);
        for (i = 0; i < nb; i++) pk_bits(p, (unsigned)(v >> i) & 1, 1);     /* N0 first */
    }
}

static size_t enc_all(const void *in, size_t n, uint8_t *out, size_t max, int flush)
{
    size_t outlen = 0, c;
    c = v44comp_encode(&enc, (const uint8_t *)in, n, out, max, &outlen);
    CHECK_EQ_I(c, n);
    if (flush) CHECK_EQ_I(v44comp_flush(&enc, out, max, &outlen), 1);
    return outlen;
}

static int dec_all(const uint8_t *in, size_t n, uint8_t *out, size_t max, size_t *outlen)
{
    size_t used = 0; int rc;
    *outlen = 0;
    rc = v44comp_decode(&dec, in, n, &used, out, max, outlen);
    if (rc == 0) CHECK_EQ_I(used, n);
    return rc;
}

static int same(const uint8_t *a, size_t na, const uint8_t *b, size_t nb)
{
    return na == nb && memcmp(a, b, na) == 0;
}

/* ---------------------------------------------------- 6.6.4, Table 6 */
static void test_table6(void)
{
    enum { A = 0x5A5, B = 0xC3, C = 0x96, D = 0x3C7, E = 0xA5, F = 0x46 };
    uint8_t out[64], want[11];
    size_t n = 0;
    packer p;

    CHECK_EQ_I(v44comp_enc_init(&enc, 2048, 255, 4096), 0);
    /* "the current codeword size C2 is 11 and the current ordinal size C5 is 8" */
    enc.c2 = 11; enc.c3 = 2048; enc.c5 = 8;
    /* two ordinals first: 18 bits, so that the example starts at bit 3 of
     * octet i as the table has it */
    v44comp_enc_put(&enc, V44COMP_CODE_ORDINAL, 0x11, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_ORDINAL, 0x22, out, sizeof out, &n);
    CHECK_EQ_I(n, 2);
    v44comp_enc_put(&enc, V44COMP_CODE_CODEWORD, A, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_ORDINAL, B, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_ORDINAL, C, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_CODEWORD, D, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_EXTLEN, 8, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_ORDINAL, E, out, sizeof out, &n);
    v44comp_enc_put(&enc, V44COMP_CODE_CONTROL, V44COMP_ETM, out, sizeof out, &n);
    CHECK_EQ_I(enc.compressed, 0);
    v44comp_enc_policy(&enc, V44COMP_NEVER);
    { uint8_t f = F; CHECK_EQ_I(v44comp_encode(&enc, &f, 1, out, sizeof out, &n), 1); }
    CHECK_EQ_I(n, 2 + 11);

    /* Table 6 row by row, bit 8 on the left; the two low bits of octet i
     * belong to the ordinal before the example (0x22: its bits 8 and 7) */
    want[0]  = (uint8_t)(((A & 0x1F) << 3) | (1 << 2) | 0);     /* A5..A1, prefix 1 */
    want[1]  = (uint8_t)((A >> 5) & 0x3F);                      /* prefix 0 0, A11..A6 */
    want[2]  = B;
    want[3]  = (uint8_t)((C & 0x7F) << 1);                      /* C7..C1, prefix 0 */
    want[4]  = (uint8_t)(((D & 0x3F) << 2) | (1 << 1) | (C >> 7));
    want[5]  = (uint8_t)((1 << 6) | ((D >> 6) & 0x1F));         /* "0", prefix 1 0, D11..D7 */
    want[6]  = (uint8_t)(((E & 1) << 7) | 0x18);                /* E1 0 0 1 1 0 0 0 */
    want[7]  = (uint8_t)(0x80 | (E >> 1));                      /* prefix 1, E8..E2 */
    want[8]  = 0;                                               /* ETM, 11 bits */
    want[9]  = 0;                                               /* ... and five 0 bits */
    want[10] = F;
    CHECK(memcmp(out + 2, want, 11) == 0);
    CHECK_EQ_I(out[2] & 3, 0);

    /* and the same through the independent packer */
    pk_init(&p);
    pk_ord(&p, 0x11, 8, 0); pk_ord(&p, 0x22, 8, 0);
    pk_cw(&p, A, 11); pk_ord(&p, B, 8, 1); pk_ord(&p, C, 8, 0); pk_cw(&p, D, 11);
    pk_ext(&p, 8, 255); pk_ord(&p, E, 8, 0); pk_cw(&p, V44COMP_ETM, 11); pk_align(&p);
    pk_octet(&p, F);
    CHECK(same(out, n, p.b, (size_t)p.n));
}

/* ------------------------------------------- Appendix II.1, Table II.1 */
static const uint8_t ii1_text[] = "ABCDEXABCDEYABCDE\377AC";
static const uint8_t ii1_octets[15] = {
    0x82, 0x84, 0x86, 0x88, 0x8A, 0xB0, 0x09, 0x29, 0x5B, 0x29, 0xF8, 0x17, 0x64, 0x68, 0x00 };

static void test_appendix_ii1(void)
{
    uint8_t out[64], back[64];
    size_t n, m, k;

    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, 3072), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    /* 7.5.1 */
    CHECK_EQ_I(enc.compressed, 1);
    CHECK_EQ_I(enc.c1, 4); CHECK_EQ_I(enc.c2, 6); CHECK_EQ_I(enc.c3, 64);
    CHECK_EQ_I(enc.c4, 0); CHECK_EQ_I(enc.c5, 7); CHECK_EQ_I(enc.esc, 0);
    CHECK_EQ_I(enc.n1, 10);
    n = enc_all(ii1_text, 20, out, sizeof out, 1);
    CHECK_EQ_I(n, 15);
    CHECK(memcmp(out, ii1_octets, 15) == 0);
    /* the node-tree of stage 11. Codeword 15 is the segment the example
     * makes "sight unseen" for the character after the flush; here it is
     * made when that character arrives. */
    CHECK_EQ_I(enc.c1, 15); CHECK_EQ_I(enc.c4, 20); CHECK_EQ_I(enc.c5, 8); CHECK_EQ_I(enc.c2, 6);
    for (k = 4; k <= 9; k++) { CHECK_EQ_I(enc.pos[k], k - 3); CHECK_EQ_I(enc.len[k], 1); }
    CHECK_EQ_I(enc.pos[10], 8);  CHECK_EQ_I(enc.len[10], 3);
    CHECK_EQ_I(enc.pos[11], 12); CHECK_EQ_I(enc.len[11], 1);
    CHECK_EQ_I(enc.pos[12], 17); CHECK_EQ_I(enc.len[12], 1);
    CHECK_EQ_I(enc.pos[13], 18); CHECK_EQ_I(enc.len[13], 1);
    CHECK_EQ_I(enc.pos[14], 19); CHECK_EQ_I(enc.len[14], 1);
    CHECK_EQ_I(enc.down[4], 10); CHECK_EQ_I(enc.down[10], 12);
    CHECK(enc.root['A'] == 14 || enc.root['A'] == 4);       /* 4 and 14 side by side */
    CHECK(enc.side[14] == 4 || enc.side[4] == 14);
    CHECK_EQ_I(enc.root['B'], 5); CHECK_EQ_I(enc.root['X'], 9); CHECK_EQ_I(enc.root['Y'], 11);
    CHECK_EQ_I(enc.root[255], 13); CHECK_EQ_I(enc.root['C'], 6);

    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec.compressed, 1);
    CHECK_EQ_I(dec.c1, 4); CHECK_EQ_I(dec.c2, 6); CHECK_EQ_I(dec.c4, 0); CHECK_EQ_I(dec.c5, 7);
    CHECK_EQ_I(dec_all(ii1_octets, 15, back, sizeof back, &m), 0);
    CHECK(same(back, m, ii1_text, 20));
    /* the string collection of stage 11 */
    CHECK_EQ_I(dec.c1, 15); CHECK_EQ_I(dec.c4, 20); CHECK_EQ_I(dec.c5, 8);
    for (k = 4; k <= 9; k++) { CHECK_EQ_I(dec.last[k], k - 3); CHECK_EQ_I(dec.slen[k], 2); }
    CHECK_EQ_I(dec.last[10], 10); CHECK_EQ_I(dec.slen[10], 5);
    CHECK_EQ_I(dec.last[11], 12); CHECK_EQ_I(dec.slen[11], 2);
    CHECK_EQ_I(dec.last[12], 17); CHECK_EQ_I(dec.slen[12], 6);
    CHECK_EQ_I(dec.last[13], 18); CHECK_EQ_I(dec.slen[13], 2);
    CHECK_EQ_I(dec.last[14], 19); CHECK_EQ_I(dec.slen[14], 2);
    CHECK_EQ_I(v44comp_dec_pending(&dec), 0);

    /* the character after the flush is appended to the "C" (codeword 15)
     * on both sides, even where a "CD" string-segment exists already
     * (6.3.2, 7.13, Table 2) */
    n = enc_all("D", 1, out, sizeof out, 1);
    CHECK_EQ_I(enc.c1, 16); CHECK_EQ_I(enc.pos[15], 20); CHECK_EQ_I(enc.len[15], 1);
    CHECK_EQ_I(dec_all(out, n, back, sizeof back, &m), 0);
    CHECK(m == 1 && back[0] == 'D');
    CHECK_EQ_I(dec.c1, 16); CHECK_EQ_I(dec.last[15], 20); CHECK_EQ_I(dec.slen[15], 2);

    /* one octet at a time into the decoder, one octet of room at a time */
    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 3072), 0);
    m = 0;
    for (k = 0; k < 15; k++) {
        size_t used = 0, pos = 0;
        while (pos < 1 || v44comp_dec_pending(&dec)) {
            size_t lim = m + 1;
            CHECK_EQ_I(v44comp_decode(&dec, ii1_octets + k + pos, 1 - pos, &used, back, lim, &m), 0);
            pos += used;
        }
    }
    CHECK(same(back, m, ii1_text, 20));
}

/* ----------------------------------------------------- Appendix II.2 */
static void test_appendix_ii2(void)
{
    uint8_t out[64], back[64];
    size_t n, m;
    packer p;

    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, 3072), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all("CCCCCCCCCCX", 11, out, sizeof out, 1);
    /* ordinal C; codeword 4 (to the decoder: its C1); string-extension
     * length 7; ordinal X; then the flush */
    pk_init(&p);
    pk_ord(&p, 'C', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 7, 255); pk_ord(&p, 'X', 7, 0);
    pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.c1, 6);
    CHECK_EQ_I(enc.pos[4], 1); CHECK_EQ_I(enc.len[4], 1); CHECK_EQ_I(enc.down[4], 5);
    CHECK_EQ_I(enc.pos[5], 3); CHECK_EQ_I(enc.len[5], 7);
    CHECK_EQ_I(enc.root['C'], 4);

    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec_all(out, n, back, sizeof back, &m), 0);
    CHECK(same(back, m, (const uint8_t *)"CCCCCCCCCCX", 11));
    CHECK_EQ_I(dec.c1, 6);
    CHECK_EQ_I(dec.last[4], 1); CHECK_EQ_I(dec.slen[4], 2);
    CHECK_EQ_I(dec.last[5], 9); CHECK_EQ_I(dec.slen[5], 9);
}

/* ------------------------------------------------- hand-made vectors */
static uint8_t big[70000], cod[160000], back[70000];

static void test_vectors(void)
{
    uint8_t out[600], bk[600];
    size_t n, m; int i, len;
    packer p;
    static const int n7s[] = { 32, 46, 47, 78, 79, 142, 143, 255 };

    /* 6.3.2, Tables 3 and 4: a run of one character is an ordinal, the
     * codeword of the 2-character string and one string-extension length;
     * every length up to N7 - 2 at each width of the last subfield. A run
     * that would pass N7 ends the extension there (the string has reached
     * N7) and the rest starts a new string. */
    for (i = 0; i < 8; i++) {
        int n7 = n7s[i];
        for (len = 1; len <= n7 + 1; len++) {
            int ext = len > n7 - 2 ? n7 - 2 : len;
            CHECK_EQ_I(v44comp_enc_init(&enc, 256, n7, 512), 0);
            v44comp_enc_policy(&enc, V44COMP_ALWAYS);
            memset(big, 'q', (size_t)len + 3); big[len + 3] = 'Z';
            n = enc_all(big, (size_t)len + 4, out, sizeof out, 1);
            pk_init(&p);
            pk_ord(&p, 'q', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, ext, n7);
            if (len == ext + 1) {
                pk_ord(&p, 'q', 7, 0);              /* one left over */
            } else if (len == ext + 2) {
                pk_cw(&p, 4, 6);                    /* "qq", then Z ends it */
            } else if (len == ext + 3) {
                pk_cw(&p, 4, 6); pk_ext(&p, 1, n7); /* "qq" + "q": a part of codeword 5 */
            } else if (len > ext) {
                CHECK(0);
            }
            if (len == ext + 2) pk_ord(&p, 'Z', 7, 1); else pk_ord(&p, 'Z', 7, 0);
            pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
            CHECK(same(out, n, p.b, (size_t)p.n));
            CHECK_EQ_I(enc.len[5], ext);
            CHECK_EQ_I(v44comp_dec_init(&dec, 256, n7, 512), 0);
            CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
            CHECK(same(bk, m, big, (size_t)len + 4));
            CHECK_EQ_I(dec.slen[5], ext + 2);
            if (len == ext + 2) {
                /* Table 2 NOTE: "qq" + Z would be 3 long, fine; but after a
                 * string of N7 characters nothing longer is made */
                CHECK_EQ_I(dec.c1, enc.c1);
            }
        }
    }

    /* 6.3.1 NOTE 1: a partial match of a string-segment is transferred as
     * a string-extension length and makes a shorter segment at that level;
     * later the longest segment that matches completely is taken, and where
     * the longer one fails part way the shorter one is */
    CHECK_EQ_I(v44comp_enc_init(&enc, 256, 255, 512), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all("abcdefg-abcdefg+abcd_abcdefg!abcd?", 34, out, sizeof out, 1);
    pk_init(&p);
    for (i = 0; i < 8; i++) pk_ord(&p, "abcdefg-"[i], 7, 0);   /* make codewords 4..11 */
    pk_cw(&p, 4, 6); pk_ext(&p, 5, 255);        /* "ab" + "cdefg": codeword 12 */
    pk_ord(&p, '+', 7, 0);                      /* 13: "+a" */
    pk_cw(&p, 4, 6); pk_ext(&p, 2, 255);        /* part of 12: "ab" + "cd": codeword 14 */
    pk_ord(&p, '_', 7, 0);                      /* 15: "_a" */
    pk_cw(&p, 12, 6);                           /* past 14, all of 12 */
    pk_ord(&p, '!', 7, 1);                      /* 16: "abcdefg!", 17: "!a" */
    pk_cw(&p, 14, 6);                           /* 12 fails at "?": back to 14, "abcd" */
    pk_ord(&p, '?', 7, 1);                      /* 18: "abcd?" */
    pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.len[12], 5); CHECK_EQ_I(enc.pos[12], 10);
    CHECK_EQ_I(enc.len[14], 2); CHECK_EQ_I(enc.pos[14], 18);
    CHECK_EQ_I(enc.c1, 19);
    CHECK_EQ_I(v44comp_dec_init(&dec, 256, 255, 512), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, (const uint8_t *)"abcdefg-abcdefg+abcd_abcdefg!abcd?", 34));
    CHECK_EQ_I(dec.c1, 19);
    CHECK_EQ_I(dec.slen[12], 7); CHECK_EQ_I(dec.slen[14], 4);
    CHECK_EQ_I(dec.slen[16], 8); CHECK_EQ_I(dec.slen[18], 5);

    /* 7.11.1: one STEPUP before the first ordinal above 127. 7.11.2:
     * codewords from 64 up need STEPUP first, once per doubling. 131
     * characters with no pair twice make codewords 4..133 and send none;
     * then the last pair again is codeword 133: two sizes up at once. */
    CHECK_EQ_I(v44comp_enc_init(&enc, 256, 255, 512), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    for (i = 0; i < 131; i++) big[i] = (uint8_t)(i < 64 ? 2 * i : 255 - 2 * (i - 64));
    big[131] = big[129]; big[132] = big[130]; big[133] = 1;
    n = enc_all(big, 134, out, sizeof out, 1);
    pk_init(&p);
    for (i = 0; i < 131; i++) {
        if (i == 64) pk_cw(&p, V44COMP_STEPUP, 6);
        pk_ord(&p, big[i], i < 64 ? 7 : 8, 0);
    }
    pk_cw(&p, V44COMP_STEPUP, 6); pk_cw(&p, V44COMP_STEPUP, 7); pk_cw(&p, 133, 8);
    pk_ord(&p, 1, 8, 1);
    pk_cw(&p, V44COMP_FLUSH, 8); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.c2, 8); CHECK_EQ_I(enc.c3, 256); CHECK_EQ_I(enc.c5, 8); CHECK_EQ_I(enc.c1, 136);
    CHECK_EQ_I(v44comp_dec_init(&dec, 256, 255, 512), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, big, 134));
    CHECK_EQ_I(dec.c2, 8); CHECK_EQ_I(dec.c5, 8); CHECK_EQ_I(dec.c1, 136);
}

/* 7.11.3 and 7.12: when the last codeword has been made the dictionary is
 * reinitialized and REINIT is transferred; 7.11.4: the same when the
 * history is full */
static void test_reinit_vectors(void)
{
    uint8_t out[1200], bk[1200];
    size_t n, m; int i;
    packer p;

    /* 256 codewords: 4..255. Characters with no pair twice make one
     * codeword each from the second on, so the 253rd makes codeword 255,
     * the last. REINIT follows the ordinal of the 252nd at once, and the
     * 253rd is the first character of the new history. */
    for (i = 0; i < 127; i++) big[i] = (uint8_t)i;                       /* k, k + 1 */
    for (i = 0; i < 127; i++) big[127 + i] = (uint8_t)((i * 5) % 127);   /* k, k + 5 */
    big[254] = 'x'; big[255] = 'y';
    CHECK_EQ_I(v44comp_enc_init(&enc, 256, 255, 4096), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all(big, 252, out, sizeof out, 0);
    CHECK_EQ_I(enc.reinits, 0); CHECK_EQ_I(enc.c1, 255); CHECK_EQ_I(enc.c4, 252);
    n = enc_all(big + 252, 1, out, sizeof out, 0);
    CHECK_EQ_I(enc.reinits, 1);
    CHECK_EQ_I(enc.c1, 4); CHECK_EQ_I(enc.c4, 1); CHECK_EQ_I(enc.hist[0], big[252]);
    CHECK_EQ_I(enc.c2, 6); CHECK_EQ_I(enc.c5, 7);

    CHECK_EQ_I(v44comp_enc_init(&enc, 256, 255, 4096), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all(big, 256, out, sizeof out, 1);
    pk_init(&p);
    for (i = 0; i < 252; i++) pk_ord(&p, big[i], 7, 0);
    pk_cw(&p, V44COMP_REINIT, 6);
    pk_ord(&p, big[252], 7, 0); pk_ord(&p, big[253], 7, 0);
    pk_ord(&p, 'x', 7, 0); pk_ord(&p, 'y', 7, 0);
    pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.reinits, 1); CHECK_EQ_I(enc.c1, 7); CHECK_EQ_I(enc.c4, 4);
    CHECK_EQ_I(v44comp_dec_init(&dec, 256, 255, 4096), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, big, 256));
    CHECK_EQ_I(dec.reinits, 1); CHECK_EQ_I(dec.c1, 7); CHECK_EQ_I(dec.c4, 4);

    /* history of 512, a run of one character. Ordinal; codeword 4 ("kk",
     * the decoder's C1) and 253 more: codeword 5, 256 characters so far.
     * The 257th starts again: "kk" and all of codeword 5 are a string of
     * 255, the longest there may be, so the 512th character is not
     * appended to it and goes as an ordinal when the history is full.
     * REINIT; the 513th is the first of the new history. */
    CHECK_EQ_I(v44comp_enc_init(&enc, 2048, 255, 512), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    memset(big, 'k', 600);
    n = enc_all(big, 513, out, sizeof out, 1);
    pk_init(&p);
    pk_ord(&p, 'k', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 253, 255);
    pk_cw(&p, 5, 6);
    pk_ord(&p, 'k', 7, 1);
    pk_cw(&p, V44COMP_REINIT, 6);
    pk_ord(&p, 'k', 7, 0);
    pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.reinits, 1); CHECK_EQ_I(enc.c4, 1); CHECK_EQ_I(enc.c1, 4);
    CHECK_EQ_I(v44comp_dec_init(&dec, 2048, 255, 512), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, big, 513));
    CHECK_EQ_I(dec.c4, 1); CHECK_EQ_I(dec.c1, 4);
}

/* 6.5, 7.14: transparent mode and the ESCAPE */
static void test_transparent(void)
{
    uint8_t out[600], bk[600];
    size_t n, m; int i;
    packer p;
    static const uint8_t e3[] = { 'a', 0, 'b', 51, 0, 51, 102, 'c' };

    /* policy "never": ETM before anything else, then the characters as they
     * are; the ESCAPE in the data is followed by EID and moves on by 51 */
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, 3072), 0);
    v44comp_enc_policy(&enc, V44COMP_NEVER);
    n = enc_all(e3, sizeof e3, out, sizeof out, 1);
    pk_init(&p);
    pk_cw(&p, V44COMP_ETM, 6); pk_align(&p);
    pk_octet(&p, 'a'); pk_octet(&p, 0); pk_octet(&p, V44COMP_EID);
    pk_octet(&p, 'b'); pk_octet(&p, 51); pk_octet(&p, V44COMP_EID);
    pk_octet(&p, 0); pk_octet(&p, 51);
    pk_octet(&p, 102); pk_octet(&p, V44COMP_EID); pk_octet(&p, 'c');
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.esc, 153); CHECK_EQ_I(enc.compressed, 0);
    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, e3, sizeof e3));
    CHECK_EQ_I(dec.esc, 153); CHECK_EQ_I(dec.compressed, 0);
    /* 6.5: the decoder dictionary is not touched in transparent mode */
    CHECK_EQ_I(dec.c1, 4); CHECK_EQ_I(dec.c4, 0);

    /* back to compressed mode: ESCAPE, ECM, a fresh dictionary, the first
     * code prefix at bit 1 of the next octet (6.5.2, 6.6) */
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all("\231xyxyxy", 7, out, sizeof out, 1);
    pk_init(&p);
    pk_octet(&p, 153); pk_octet(&p, V44COMP_ECM);
    pk_cw(&p, V44COMP_STEPUP, 6); pk_ord(&p, 0x99, 8, 0);   /* no EID in compressed mode */
    pk_ord(&p, 'x', 8, 0); pk_ord(&p, 'y', 8, 0);
    pk_cw(&p, 5, 6); pk_ext(&p, 2, 255);
    pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(enc.esc, 153);                   /* 7.14: not changed in compressed mode */
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, (const uint8_t *)"\231xyxyxy", 7));
    CHECK_EQ_I(dec.compressed, 1); CHECK_EQ_I(dec.esc, 153);
    CHECK_EQ_I(enc.mode_switches, 2);

    /* and out again (6.5.1): ETM in the current codeword size, 0 bits to
     * the octet boundary, then the characters as they are */
    v44comp_enc_policy(&enc, V44COMP_NEVER);
    n = enc_all("xyxyZ", 5, out, sizeof out, 1);
    pk_init(&p);
    pk_cw(&p, V44COMP_ETM, 6); pk_align(&p);
    pk_octet(&p, 'x'); pk_octet(&p, 'y'); pk_octet(&p, 'x'); pk_octet(&p, 'y'); pk_octet(&p, 'Z');
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, (const uint8_t *)"xyxyZ", 5));

    /* 6.5.1 a): a string match that is held goes out by the normal
     * procedures before ETM (the control code prefix after a codeword) */
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, 3072), 0);
    v44comp_enc_policy(&enc, V44COMP_ALWAYS);
    n = enc_all("abab", 4, out, sizeof out, 0);
    v44comp_enc_policy(&enc, V44COMP_NEVER);
    CHECK_EQ_I(v44comp_encode(&enc, (const uint8_t *)"Z", 1, out, sizeof out, &n), 1);
    pk_init(&p);
    pk_ord(&p, 'a', 7, 0); pk_ord(&p, 'b', 7, 0); pk_cw(&p, 4, 6);
    pk_cw(&p, V44COMP_ETM, 6); pk_align(&p);
    pk_octet(&p, 'Z');
    CHECK(same(out, n, p.b, (size_t)p.n));
    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec_all(out, n, bk, sizeof bk, &m), 0);
    CHECK(same(bk, m, (const uint8_t *)"ababZ", 5));

    /* every octet value through transparent mode, twice over */
    CHECK_EQ_I(v44comp_enc_init(&enc, 256, 32, 512), 0);
    v44comp_enc_policy(&enc, V44COMP_NEVER);
    for (i = 0; i < 512; i++) big[i] = (uint8_t)(i * 51);
    n = enc_all(big, 512, cod, sizeof cod, 1);
    CHECK(n > 512 + 1 && n <= 2 * 512 + 1);
    CHECK_EQ_I(v44comp_dec_init(&dec, 256, 32, 512), 0);
    CHECK_EQ_I(dec_all(cod, n, back, sizeof back, &m), 0);
    CHECK(same(back, m, big, 512));
    CHECK_EQ_I(enc.esc, dec.esc);
}

/* ---------------------------------------------------- decoder errors */
static int feed(const packer *p, int n2, int n7, int n8)
{
    uint8_t bk[8192]; size_t m, used = 0; int rc;
    m = 0;
    CHECK_EQ_I(v44comp_dec_init(&dec, n2, n7, n8), 0);
    rc = v44comp_decode(&dec, p->b, (size_t)p->n, &used, bk, sizeof bk, &m);
    /* sticky */
    if (rc) CHECK_EQ_I(v44comp_decode(&dec, p->b, 1, &used, bk, sizeof bk, &m), rc);
    return rc;
}

static void test_decoder_errors(void)
{
    packer p; int i;

    CHECK_EQ_I(v44comp_enc_init(&enc, 255, 255, 4096), -1);
    CHECK_EQ_I(v44comp_enc_init(&enc, V44COMP_MAX_CODEWORDS + 1, 255, 4096), -1);
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 31, 4096), -1);
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 256, 4096), -1);
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, 511), -1);
    CHECK_EQ_I(v44comp_enc_init(&enc, 1024, 255, V44COMP_MAX_HISTORY + 1), -1);
    CHECK_EQ_I(v44comp_dec_init(&dec, 255, 255, 4096), -1);
    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 31, 4096), -1);
    CHECK_EQ_I(v44comp_dec_init(&dec, 1024, 255, 511), -1);
    CHECK_EQ_I(v44comp_dec_init(&dec, V44COMP_MAX_CODEWORDS, 255, V44COMP_MAX_HISTORY), 0);
    CHECK_EQ_I(v44comp_dec_init(&dec, 256, 32, 512), 0);

    /* 7.15: a codeword greater than C1 */
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_ord(&p, 'b', 7, 0); pk_cw(&p, 6, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_CODEWORD);
    /* ... equal to C1 is fine after an ordinal or a codeword (6.4.1) */
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_ord(&p, 'b', 7, 0); pk_cw(&p, 5, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec.c4, 4); CHECK(memcmp(dec.hist, "abbb", 4) == 0);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_ord(&p, 'b', 7, 0); pk_cw(&p, 4, 6); pk_cw(&p, 6, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec.c4, 7); CHECK(memcmp(dec.hist, "abababa", 7) == 0);
    CHECK_EQ_I(dec.slen[6], 3); CHECK_EQ_I(dec.last[6], 4);
    /* ... but not as the very first code, nor after a string-extension
     * length, nor after REINIT: there is nothing to build it from */
    pk_init(&p); pk_cw(&p, 4, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_CODEWORD);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 3, 255); pk_cw(&p, 5, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), 0);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 3, 255); pk_cw(&p, 6, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_CODEWORD);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, V44COMP_REINIT, 6); pk_cw(&p, 4, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_CODEWORD);
    /* FLUSH and STEPUP in between change nothing (Table 2) */
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, V44COMP_FLUSH, 6); pk_align(&p);
    pk_cw(&p, 4, 6); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), 0);
    CHECK_EQ_I(dec.c4, 3); CHECK_EQ_I(dec.c1, 5);

    /* 7.15: STEPUP past N1 (8 bits for 256 codewords) */
    pk_init(&p); pk_ord(&p, 'a', 7, 0);
    pk_cw(&p, V44COMP_STEPUP, 6); pk_cw(&p, V44COMP_STEPUP, 7); pk_cw(&p, V44COMP_FLUSH, 8); pk_align(&p);
    CHECK_EQ_I(feed(&p, 256, 255, 3072), 0);
    CHECK_EQ_I(dec.c2, 8);
    pk_init(&p); pk_ord(&p, 'a', 7, 0);
    pk_cw(&p, V44COMP_STEPUP, 6); pk_cw(&p, V44COMP_STEPUP, 7); pk_cw(&p, V44COMP_STEPUP, 8);
    pk_cw(&p, V44COMP_FLUSH, 9); pk_align(&p);
    CHECK_EQ_I(feed(&p, 256, 255, 3072), V44COMP_ERR_STEPUP);
    /* N1 for 257 codewords is 9 bits */
    CHECK_EQ_I(feed(&p, 257, 255, 3072), 0);
    CHECK_EQ_I(dec.c2, 9);
    /* 7.15: STEPUP of the ordinal size past 8 */
    pk_init(&p); pk_cw(&p, V44COMP_STEPUP, 6); pk_ord(&p, 200, 8, 0);
    pk_cw(&p, V44COMP_STEPUP, 6); pk_ord(&p, 201, 8, 0); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_ORDINAL);
    CHECK_EQ_I(dec.c4, 1);

    /* transparent mode: EPM (parameter mode is not supported) and the
     * reserved command codes */
    for (i = 2; i < 256; i += (i < 8 ? 1 : 31)) {
        pk_init(&p); pk_cw(&p, V44COMP_ETM, 6); pk_align(&p);
        pk_octet(&p, 'a'); pk_octet(&p, 0); pk_octet(&p, i);
        CHECK_EQ_I(feed(&p, 1024, 255, 3072), V44COMP_ERR_COMMAND);
    }

    /* a string-extension length that would make a string longer than N7 */
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 30, 32); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 32, 3072), 0);
    CHECK_EQ_I(dec.c4, 33);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 31, 32); pk_align(&p);
    CHECK_EQ_I(feed(&p, 1024, 32, 3072), V44COMP_ERR_EXTEND);
    /* more characters than the history holds, with no REINIT */
    pk_init(&p);
    for (i = 0; i < 512; i++) pk_ord(&p, 'a' + i % 2 * (i % 23), 7, 0);
    pk_align(&p);
    CHECK_EQ_I(feed(&p, 2048, 255, 512), 0);
    pk_init(&p);
    for (i = 0; i < 513; i++) pk_ord(&p, 'a' + i % 2 * (i % 23), 7, 0);
    pk_align(&p);
    CHECK_EQ_I(feed(&p, 2048, 255, 512), V44COMP_ERR_HISTORY);
    pk_init(&p); pk_ord(&p, 'a', 7, 0); pk_cw(&p, 4, 6); pk_ext(&p, 253, 255);
    pk_cw(&p, 5, 6); pk_cw(&p, 5, 6); pk_align(&p);     /* 3 + 253 + 255 + 255 */
    CHECK_EQ_I(feed(&p, 2048, 255, 512), V44COMP_ERR_HISTORY);
    /* a full string collection takes no more strings and is no error by
     * itself: 256 codewords, ordinals only */
    pk_init(&p);
    for (i = 0; i < 300; i++) pk_ord(&p, i % 100, 7, 0);
    pk_align(&p);
    CHECK_EQ_I(feed(&p, 256, 255, 512), 0);
    CHECK_EQ_I(dec.c1, 256);
}

/* Anything at all into the decoder: it must stay inside its arrays and its
 * output buffer whatever comes, and either decode or report C-ERROR. The
 * decoder sits between guard areas that are checked afterwards. */
static struct { uint8_t g0[64]; v44comp_dec_t d; uint8_t g1[64]; uint8_t o[300]; uint8_t g2[64]; } box;

static void test_garbage(void)
{
    static const int n2s[] = { 256, 300, 512, 1024, 2048 };
    static const int n7s[] = { 32, 40, 100, 255 };
    static const int n8s[] = { 512, 1000, 4096 };
    int round, errors = 0, clean = 0;

    for (round = 0; round < 3000; round++) {
        int n2 = n2s[rnd() % 5], n7, n8, kind, steps, s, rc = 0;
        size_t total = 0;
        n7 = n7s[rnd() % 4]; n8 = n8s[rnd() % 3];
        kind = (int)(rnd() % 4); steps = 1 + (int)(rnd() % 60);
        if (n2 > V44COMP_MAX_CODEWORDS) n2 = V44COMP_MAX_CODEWORDS;
        if (n8 > V44COMP_MAX_HISTORY) n8 = V44COMP_MAX_HISTORY;
        memset(box.g0, 0xA5, sizeof box.g0); memset(box.g1, 0xA5, sizeof box.g1);
        memset(box.g2, 0xA5, sizeof box.g2);
        CHECK_EQ_I(v44comp_dec_init(&box.d, n2, n7, n8), 0);
        for (s = 0; s < steps && rc == 0; s++) {
            uint8_t in[64]; size_t len = 1 + rnd() % 64, used = 0, m = 0, max, i;
            max = rnd() % 301;
            for (i = 0; i < len; i++) {
                /* random octets; mostly ones (codewords and control codes);
                 * mostly zeros (ordinals, ETM); a valid stream with errors */
                unsigned r = rnd(), q = rnd();
                q &= rnd();
                in[i] = kind == 0 ? (uint8_t)r : kind == 1 ? (uint8_t)(r | q)
                      : kind == 2 ? (uint8_t)(r & q) : (uint8_t)(r % 7 ? 0x55 : r);
            }
            for (;;) {
                size_t u = 0;
                m = 0;
                rc = v44comp_decode(&box.d, in + used, len - used, &u, box.o, max, &m);
                CHECK(m <= max); CHECK(u <= len - used);
                used += u; total += m;
                if (rc != 0 || (u == 0 && m == 0)) break;
            }
            CHECK(box.d.c1 <= box.d.n2); CHECK(box.d.c4 <= box.d.n8); CHECK(box.d.c2 <= box.d.n1);
            CHECK(box.d.c5 <= 8); CHECK(box.d.done <= box.d.c4);
        }
        if (rc) errors++; else clean++;
        {
            size_t i; int ok = 1;
            for (i = 0; i < 64; i++) if (box.g0[i] != 0xA5 || box.g1[i] != 0xA5 || box.g2[i] != 0xA5) ok = 0;
            CHECK(ok);
        }
        (void)total;
    }
    CHECK(errors > 500);
    CHECK(clean > 20);
}

/* -------------------------------------------------------- data kinds */
static const char *words[] = {
    "the", "of", "and", "to", "in", "is", "that", "for", "it", "as", "was", "with", "be", "by", "on",
    "not", "he", "this", "are", "or", "his", "from", "at", "which", "but", "have", "an", "had", "they",
    "you", "were", "their", "one", "all", "we", "can", "her", "has", "there", "been", "if", "more",
    "when", "will", "would", "who", "so", "no", "modem", "carrier", "signal", "error", "frame",
    "window", "answer", "originate", "telephone", "line", "data", "octet", "phase", "symbol", "rate",
    "retrain", "compression", "dictionary", "history", "codeword", "string", "character", "network",
    "connection", "server", "download", "people", "time", "because", "between", "through", "after",
    "before", "should", "another", "example", "following", "information", "different", "possible" };
#define NWORDS ((int)(sizeof words / sizeof words[0]))

static int pick_word(void)
{
    /* low numbers much more often than high ones */
    int a = (int)(rnd() % NWORDS), b, c;
    b = (int)(rnd() % NWORDS); c = (int)(rnd() % NWORDS);
    return a < b ? (a < c ? a : c) : (b < c ? b : c);
}

static size_t put_str(uint8_t *d, size_t i, size_t n, const char *s)
{
    while (*s && i < n) d[i++] = (uint8_t)*s++;
    return i;
}

/* English-like: sentences of common words */
static void fill_text(uint8_t *d, size_t n)
{
    size_t i = 0; int inword = 0;
    while (i < n) {
        const char *w = words[pick_word()];
        if (inword == 0 && i < n) { d[i++] = (uint8_t)(w[0] - 32); w++; }
        i = put_str(d, i, n, w);
        inword++;
        if (i >= n) break;
        if (inword > 4 && rnd() % 9 == 0) { i = put_str(d, i, n, rnd() % 5 ? ". " : ".\r\n\r\n"); inword = 0; }
        else if (rnd() % 14 == 0) i = put_str(d, i, n, ", ");
        else d[i++] = ' ';
    }
}

/* HTML-like: markup of the dial-up years around the same words */
static void fill_html(uint8_t *d, size_t n)
{
    static const char *tags[] = { "p", "b", "i", "td", "li", "font", "a", "h2", "center", "tt" };
    static const char *open_extra[] = { "", "", " align=\"center\"", " class=\"nav\"", " width=\"100%\"",
        " face=\"Arial, Helvetica\" size=\"2\"", " href=\"http://www.example.com/", " bgcolor=\"#" };
    size_t i = 0;
    char num[16];
    i = put_str(d, i, n, "<html>\r\n<head><title>Example page</title></head>\r\n<body bgcolor=\"#FFFFFF\">\r\n");
    while (i < n) {
        int t = (int)(rnd() % 10), x, k, w;
        x = (int)(rnd() % 8); w = 1 + (int)(rnd() % 12);
        if (rnd() % 6 == 0) i = put_str(d, i, n, "<tr>\r\n  ");
        i = put_str(d, i, n, "<"); i = put_str(d, i, n, tags[t]); i = put_str(d, i, n, open_extra[x]);
        if (x == 6) {
            i = put_str(d, i, n, words[pick_word()]); i = put_str(d, i, n, "/");
            i = put_str(d, i, n, words[pick_word()]);
            snprintf(num, sizeof num, "%u", rnd() % 1000); i = put_str(d, i, n, num);
            i = put_str(d, i, n, ".html\"");
        } else if (x == 7) {
            unsigned hi = rnd() << 9;
            snprintf(num, sizeof num, "%06X\"", (hi ^ rnd()) & 0xFFFFFF); i = put_str(d, i, n, num);
        }
        i = put_str(d, i, n, ">");
        for (k = 0; k < w; k++) {
            i = put_str(d, i, n, words[pick_word()]);
            if (k + 1 < w) i = put_str(d, i, n, rnd() % 17 ? " " : "&nbsp;");
        }
        i = put_str(d, i, n, "</"); i = put_str(d, i, n, tags[t]); i = put_str(d, i, n, ">");
        if (rnd() % 3 == 0) i = put_str(d, i, n, "\r\n");
        if (rnd() % 9 == 0) i = put_str(d, i, n, "<br>\r\n");
        if (rnd() % 23 == 0) {
            i = put_str(d, i, n, "<img src=\"/images/");
            i = put_str(d, i, n, words[pick_word()]);
            snprintf(num, sizeof num, "%u", rnd() % 100); i = put_str(d, i, n, num);
            i = put_str(d, i, n, ".gif\" width=\"");
            snprintf(num, sizeof num, "%u", 10 + rnd() % 400); i = put_str(d, i, n, num);
            i = put_str(d, i, n, "\" height=\"");
            snprintf(num, sizeof num, "%u", 10 + rnd() % 300); i = put_str(d, i, n, num);
            i = put_str(d, i, n, "\" border=\"0\">\r\n");
        }
    }
}

static void fill_random(uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) d[i] = (uint8_t)(rnd() >> 3);
}

/* binary with structure: records of counters, small numbers and noise */
static void fill_binary(uint8_t *d, size_t n)
{
    size_t i; unsigned seq = 0;
    for (i = 0; i < n; i++) {
        unsigned f = (unsigned)(i % 24);
        if (f == 0) seq++;
        d[i] = f < 2 ? (uint8_t)(0x45 + f * 0xBB) : f < 4 ? (uint8_t)(seq >> (8 * (3 - f)))
             : f < 8 ? 0 : f < 12 ? (uint8_t)(rnd() % 4) : f < 16 ? (uint8_t)(0xC0 + f)
             : f < 20 ? (uint8_t)(rnd() >> 4) : 0xFF;
    }
}

static void fill_runs(uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t top = rnd() % 4 ? 40 : 1200, r = 1 + rnd() % top;
        uint8_t c = (uint8_t)(rnd() >> 4);
        while (r-- && i < n) d[i++] = c;
    }
}

/* all 256 values: in order, backwards, and as pairs that never repeat */
static void fill_all256(uint8_t *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned blk = (unsigned)(i / 256), k = (unsigned)(i % 256);
        d[i] = blk % 3 == 0 ? (uint8_t)k : blk % 3 == 1 ? (uint8_t)(255 - k) : (uint8_t)(k * (2 * blk + 1));
    }
}

/* a little of everything, in pieces of uneven length */
static void fill_mixed(uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t r = 1 + rnd() % 3000;
        if (r > n - i) r = n - i;
        switch (rnd() % 6) {
        case 0: fill_text(d + i, r); break;
        case 1: fill_html(d + i, r); break;
        case 2: fill_random(d + i, r); break;
        case 3: fill_binary(d + i, r); break;
        case 4: fill_runs(d + i, r); break;
        default: fill_all256(d + i, r); break;
        }
        i += r;
    }
}

typedef void (*fill_fn)(uint8_t *, size_t);
static const struct { const char *name; fill_fn fn; } kinds[] = {
    { "text", fill_text }, { "html", fill_html }, { "random", fill_random }, { "binary", fill_binary },
    { "runs", fill_runs }, { "all256", fill_all256 }, { "mixed", fill_mixed } };
#define NKINDS 7

/* ------------------------------------------------------- round trips */
/* Encode big[0..n) and decode it again. Input goes in pieces of 1..chunk
 * characters; every flush_every characters (0 = only at the end) a flush,
 * after which the decoder must have delivered everything so far (7.13).
 * tight != 0 gives both sides output buffers of a few octets at a time.
 * Returns the number of encoded octets. */
static size_t roundtrip(size_t n, int n2, int n7, int n8, int d2, int d7, int d8,
                        int policy, size_t chunk, size_t flush_every, int tight)
{
    size_t in = 0, ncod = 0, fed = 0, nback = 0, since = 0;
    int rc = 0, ok = 1;

    CHECK_EQ_I(v44comp_enc_init(&enc, n2, n7, n8), 0);
    CHECK_EQ_I(v44comp_dec_init(&dec, d2, d7, d8), 0);
    v44comp_enc_policy(&enc, policy);
    while (in < n || since > 0) {
        size_t c = 1 + rnd() % chunk, room, used;
        int flush = 0;
        if (c > n - in) c = n - in;
        if (flush_every && since + c >= flush_every) { c = flush_every - since; flush = 1; }
        if (c > n - in) c = n - in;
        if (in + c == n) flush = 1;
        /* encoder */
        for (used = 0; used < c; ) {
            size_t lim = tight ? ncod + 1 + rnd() % 7 : sizeof cod;
            used += v44comp_encode(&enc, big + in + used, c - used, cod, lim, &ncod);
            if (ncod + 400 > sizeof cod) { CHECK(0); return 0; }
        }
        in += c; since += c;
        if (flush) {
            for (;;) {
                size_t lim = tight ? ncod + 1 + rnd() % 7 : sizeof cod;
                if (v44comp_flush(&enc, cod, lim, &ncod)) break;
            }
            CHECK_EQ_I(v44comp_enc_pending(&enc), 0);
            since = 0;
        }
        /* decoder: all the octets there are, in pieces */
        while (fed < ncod || v44comp_dec_pending(&dec)) {
            size_t piece = tight ? 1 + rnd() % 5 : ncod - fed, lim;
            if (piece > ncod - fed) piece = ncod - fed;
            room = tight ? 1 + rnd() % 9 : sizeof back - nback;
            lim = nback + room > sizeof back ? sizeof back : nback + room;
            rc = v44comp_decode(&dec, cod + fed, piece, &used, back, lim, &nback);
            fed += used;
            if (rc != 0) break;
        }
        if (rc != 0) break;
        if (flush) {
            if (nback != in) ok = 0;
            if (enc.compressed != dec.compressed || enc.esc != dec.esc) ok = 0;
            if (enc.compressed && (enc.c1 != dec.c1 || enc.c2 != dec.c2 || enc.c5 != dec.c5)) ok = 0;
            if (!ok) break;
        }
    }
    CHECK_EQ_I(rc, 0);
    CHECK(ok);
    CHECK_EQ_I(nback, n);
    CHECK(nback == n && memcmp(back, big, n) == 0);
    CHECK_EQ_I(enc.in_chars, n);
    CHECK_EQ_I(enc.out_octets, ncod);
    CHECK_EQ_I(dec.in_octets, ncod);
    CHECK_EQ_I(dec.out_chars, n);
    return ncod;
}

static void test_roundtrips(void)
{
    static const int n2s[] = { 256, 512, 1024, 2048 };
    static const int n8s[] = { 512, 768, 1024, 2048, 3072, 4096 };
    int k, a, b, c, pol;
    size_t n, i;

    /* tiny inputs: nothing, one character, a few */
    for (n = 0; n <= 6; n++) {
        for (k = 0; k < 4; k++) {
            for (i = 0; i < n; i++) big[i] = k == 0 ? 'a' : k == 1 ? (uint8_t)(200 + i) : k == 2 ? (uint8_t)("abab"[i % 4]) : 0;
            for (pol = V44COMP_AUTO; pol <= V44COMP_NEVER; pol++)
                roundtrip(n, 2048, 255, 4096, 2048, 255, 4096, pol, 3, 0, 0);
        }
    }
    CHECK_EQ_I(v44comp_enc_init(&enc, 2048, 255, 4096), 0);
    { size_t m = 0; CHECK_EQ_I(v44comp_flush(&enc, cod, sizeof cod, &m), 1); CHECK_EQ_I(m, 0); }

    /* every kind of data at every corner of the parameters */
    for (k = 0; k < NKINDS; k++) {
        kinds[k].fn(big, 30000);
        for (a = 0; a < 4; a++) for (b = 0; b < 2; b++) for (c = 0; c < 6; c++) {
            int n2 = n2s[a], n7 = b ? 255 : 32, n8 = n8s[c];
            if (n2 > V44COMP_MAX_CODEWORDS || n8 > V44COMP_MAX_HISTORY) continue;
            size_t len = 12000 + rnd() % 8000, fe;
            int tight;
            roundtrip(len, n2, n7, n8, n2, n7, n8, V44COMP_ALWAYS, 700, 0, 0);
            CHECK(enc.reinits >= 2);            /* node-tree full or history full, many times */
            len = 4000 + rnd() % 3000; fe = 1 + rnd() % 600; tight = (int)(rnd() % 2);
            roundtrip(len, n2, n7, n8, n2, n7, n8, V44COMP_AUTO, 300, fe, tight);
        }
        /* odd values in between, whole length */
        roundtrip(30000, 1525 > V44COMP_MAX_CODEWORDS ? V44COMP_MAX_CODEWORDS : 1525, 100, 3000 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 3000,
                  1525 > V44COMP_MAX_CODEWORDS ? V44COMP_MAX_CODEWORDS : 1525, 100, 3000 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 3000,
                  V44COMP_AUTO, 5000, 0, 0);
        roundtrip(30000, 257, 33, 513, 257, 33, 513, V44COMP_ALWAYS, 5000, 0, 0);
        roundtrip(20000, 300, 46, 600, 300, 46, 600, V44COMP_ALWAYS, 100, 0, 1);
        roundtrip(20000, 300, 47, 600, 300, 47, 600, V44COMP_ALWAYS, 100, 77, 1);
        roundtrip(20000, 700, 78, 900, 700, 78, 900, V44COMP_ALWAYS, 100, 0, 1);
        roundtrip(20000, 700, 79, 900, 700, 79, 900, V44COMP_AUTO, 100, 500, 1);
        roundtrip(20000, 700, 142, 900, 700, 142, 900, V44COMP_ALWAYS, 100, 0, 1);
        roundtrip(20000, 700, 143, 900, 700, 143, 900, V44COMP_NEVER, 100, 0, 1);
    }

    /* a flush after every 1..200 characters, on text, HTML and mixed data */
    for (k = 0; k < 3; k++) {
        size_t fe;
        kinds[k == 2 ? 6 : k].fn(big, 9000);
        for (fe = 1; fe <= 200; fe++) {
            int n2 = n2s[fe % 4] > V44COMP_MAX_CODEWORDS ? V44COMP_MAX_CODEWORDS : n2s[fe % 4];
            roundtrip(fe < 20 ? 1500 : 6000 + fe, n2, fe % 3 ? 255 : 32, fe % 5 ? 4096 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 4096 : 512,
                      n2, fe % 3 ? 255 : 32, fe % 5 ? 4096 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 4096 : 512,
                      fe % 7 == 0 ? V44COMP_AUTO : V44COMP_ALWAYS, 1 + fe / 2, fe, (int)(fe & 1));
        }
    }

    /* one character per call, a flush at random points */
    for (k = 0; k < NKINDS; k++) {
        kinds[k].fn(big, 6000);
        n = 1 + rnd() % 300;
        roundtrip(6000, 512, 32, 1024, 512, 32, 1024, V44COMP_AUTO, 1, n, 1);
        roundtrip(6000, 2048 > V44COMP_MAX_CODEWORDS ? V44COMP_MAX_CODEWORDS : 2048, 255, 4096 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 4096,
                  2048 > V44COMP_MAX_CODEWORDS ? V44COMP_MAX_CODEWORDS : 2048, 255, 4096 > V44COMP_MAX_HISTORY ? V44COMP_MAX_HISTORY : 4096,
                  V44COMP_ALWAYS, 1, 0, 0);
    }

    /* long inputs with random everything */
    for (k = 0; k < 40; k++) {
        int n2 = 256 + (int)(rnd() % (V44COMP_MAX_CODEWORDS - 255)), n7, n8, tight;
        size_t len, chunk, fe;
        n7 = 32 + (int)(rnd() % 224);
        n8 = 512 + (int)(rnd() % (V44COMP_MAX_HISTORY - 511));
        kinds[k % NKINDS].fn(big, 60000);
        len = 20000 + rnd() % 40000; pol = (int)(rnd() % 3); chunk = 1 + rnd() % 2000;
        fe = rnd() % 3 ? 0 : 1; if (fe) fe = 1 + rnd() % 5000;
        tight = rnd() % 4 == 0;
        roundtrip(len, n2, n7, n8, n2, n7, n8, pol, chunk, fe, tight);
    }
}

/* 7.11.5: the encoder leaves compressed mode on data that does not compress
 * and comes back when it does */
static void test_mode_switches(void)
{
    size_t n;

    fill_random(big, 40000);
    n = roundtrip(40000, 2048, 255, 4096, 2048, 255, 4096, V44COMP_AUTO, 500, 0, 0);
    CHECK(enc.mode_switches >= 1);
    CHECK_EQ_I(enc.compressed, 0);
    CHECK(n < 40000 + 40000 / 50);              /* under 2 % over */
    n = roundtrip(40000, 2048, 255, 4096, 2048, 255, 4096, V44COMP_ALWAYS, 500, 0, 0);
    CHECK(n > 40000);                           /* what it would have cost */
    CHECK_EQ_I(enc.mode_switches, 0);

    fill_text(big, 20000); fill_random(big + 20000, 20000); fill_text(big + 40000, 20000);
    n = roundtrip(60000, 2048, 255, 4096, 2048, 255, 4096, V44COMP_AUTO, 500, 0, 0);
    CHECK(enc.mode_switches >= 2);
    CHECK_EQ_I(enc.compressed, 1);
    CHECK(n < 20000 + 20000 / 25 + 2 * 12000);

    fill_text(big, 60000);
    n = roundtrip(60000, 2048, 255, 4096, 2048, 255, 4096, V44COMP_AUTO, 500, 0, 0);
    CHECK_EQ_I(enc.mode_switches, 0);
    CHECK(n < 60000 / 2);
    n = roundtrip(60000, 2048, 255, 4096, 2048, 255, 4096, V44COMP_NEVER, 500, 0, 0);
    CHECK_EQ_I(n, 60000 + 1);                   /* the ETM octet; text has no ESCAPE in it */
    CHECK_EQ_I(enc.mode_switches, 1);
}

/* --------------------------------------------- figures (printed only) */
static v42comp_enc_t benc;

static size_t bis_size(const uint8_t *src, size_t n, int n2, int n7)
{
    size_t in = 0, total = 0;
    if (n2 > V42COMP_MAX_CODEWORDS || v42comp_enc_init(&benc, n2, n7) != 0) return 0;
    while (in < n) {
        size_t m = 0;
        in += v42comp_encode(&benc, src + in, n - in, cod, sizeof cod, &m);
        total += m;
    }
    for (;;) {
        size_t m = 0; int done = v42comp_flush(&benc, cod, sizeof cod, &m);
        total += m;
        if (done) break;
    }
    return total;
}

static size_t v44_size(const uint8_t *src, size_t n, int n2, int n7, int n8)
{
    size_t in = 0, total = 0;
    if (n2 > V44COMP_MAX_CODEWORDS || n8 > V44COMP_MAX_HISTORY || v44comp_enc_init(&enc, n2, n7, n8) != 0) return 0;
    while (in < n) {
        size_t m = 0;
        in += v44comp_encode(&enc, src + in, n - in, cod, sizeof cod, &m);
        total += m;
    }
    for (;;) {
        size_t m = 0; int done = v44comp_flush(&enc, cod, sizeof cod, &m);
        total += m;
        if (done) break;
    }
    return total;
}

static void ratio(char *s, size_t max, size_t n, size_t out)
{
    if (out == 0) snprintf(s, max, "   n/a");
    else snprintf(s, max, "%3lu.%02lu", (unsigned long)(n / out), (unsigned long)(n * 100 / out % 100));
}

static uint8_t sample[300000];

static void figures_for(const char *name, const uint8_t *src, size_t n)
{
    char a[16], b[16], c[16], d[16], e[16], f[16];
    ratio(a, sizeof a, n, bis_size(src, n, 512, 32));
    ratio(b, sizeof b, n, bis_size(src, n, 2048, 32));
    ratio(c, sizeof c, n, v44_size(src, n, 512, 32, 1024));
    ratio(d, sizeof d, n, v44_size(src, n, 1024, 255, 3072));
    ratio(e, sizeof e, n, v44_size(src, n, 2048, 32, 4096));
    ratio(f, sizeof f, n, v44_size(src, n, 2048, 255, 4096));
    printf("  %-22s %7lu | %s  %s | %s  %s  %s  %s\n", name, (unsigned long)n, a, b, c, d, e, f);
}

static void figures(void)
{
    static const char *files[2][2] = {
        { "html.bin (file)", "/tmp/v90d-scratch/tp/html.bin" },
        { "text.bin (file)", "/tmp/v90d-scratch/tp/text.bin" } };
    int k;
    clock_t t0, t1;
    size_t n, in, total, m, used, nb;
    double se, sd;

    printf("  compression ratio (characters in / octets out), mode chosen by the encoder\n");
    printf("  %-22s %7s | %s | %s\n", "", "", "V.42 bis      ", "V.44");
    printf("  %-22s %7s | %s | %s\n", "data", "octets", "512/32 2048/32", "512/32/1024 1024/255/3072 2048/32/4096 2048/255/4096");
    rnd_state = 2026;
    for (k = 0; k < NKINDS; k++) {
        char nm[32];
        kinds[k].fn(sample, sizeof sample);
        snprintf(nm, sizeof nm, "%s (generated)", kinds[k].name);
        figures_for(nm, sample, sizeof sample);
    }
    for (k = 0; k < 2; k++) {
        FILE *fp = fopen(files[k][1], "rb");
        if (!fp) continue;
        n = fread(sample, 1, sizeof sample, fp);
        fclose(fp);
        if (n > 0) figures_for(files[k][0], sample, n);
    }

    /* speed: HTML-like data through the encoder and back through the decoder */
    rnd_state = 99;
    fill_html(sample, sizeof sample);
    if (v44comp_enc_init(&enc, V44COMP_MAX_CODEWORDS, 255, V44COMP_MAX_HISTORY) != 0) return;
    if (v44comp_dec_init(&dec, V44COMP_MAX_CODEWORDS, 255, V44COMP_MAX_HISTORY) != 0) return;
    n = 0;
    t0 = clock();
    for (k = 0; k < 8; k++) {
        for (in = 0, total = 0; in < sizeof sample; ) {
            m = 0;
            in += v44comp_encode(&enc, sample + in, sizeof sample - in, cod, sizeof cod, &m);
            total += m;
        }
        n += sizeof sample;
    }
    t1 = clock();
    se = (double)(t1 - t0) / CLOCKS_PER_SEC;
    /* one more pass kept whole for the decoder */
    CHECK_EQ_I(v44comp_enc_init(&enc, V44COMP_MAX_CODEWORDS, 255, V44COMP_MAX_HISTORY), 0);
    total = 0;
    in = v44comp_encode(&enc, sample, 60000, cod, sizeof cod, &total);
    CHECK_EQ_I(in, 60000);
    CHECK_EQ_I(v44comp_flush(&enc, cod, sizeof cod, &total), 1);
    t0 = clock();
    for (k = 0; k < 40; k++) {
        CHECK_EQ_I(v44comp_dec_init(&dec, V44COMP_MAX_CODEWORDS, 255, V44COMP_MAX_HISTORY), 0);
        nb = 0;
        v44comp_decode(&dec, cod, total, &used, back, sizeof back, &nb);
    }
    t1 = clock();
    sd = (double)(t1 - t0) / CLOCKS_PER_SEC;
    CHECK_EQ_I(nb, 60000);
    printf("  speed on HTML-like data: encoder %.0f characters/s, decoder %.0f characters/s\n",
           se > 0 ? (double)n / se : 0.0, sd > 0 ? 40.0 * 60000.0 / sd : 0.0);
    printf("  sizes: encoder %lu octets, decoder %lu octets\n",
           (unsigned long)sizeof(v44comp_enc_t), (unsigned long)sizeof(v44comp_dec_t));
}

int main(void)
{
    test_table6();
    test_appendix_ii1();
    test_appendix_ii2();
    test_vectors();
    test_reinit_vectors();
    test_transparent();
    test_decoder_errors();
    test_garbage();
    test_roundtrips();
    test_mode_switches();
    figures();
    T_REPORT("test_v44");
}
