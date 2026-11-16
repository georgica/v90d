/* Test side only: ratio and cost figures for the packet compressors on a
 * file, cut into TCP-like packets (40 octets of header whose sequence
 * number and checksum move, 1460 of the file).
 *
 *   comp_bench FILE                 the table: Deflate at every window, MPPC
 *   comp_bench FILE d W [N [CHAIN [WORK [LAZY [NICE]]]]]
 *                                   Deflate only, window W, the first N
 *                                   packets (0 = all), for instruction counts
 *   comp_bench FILE m [N]           MPPC only
 *   comp_bench FILE i W [N]         Deflate, then decompress (decoder cost: i minus d)
 *   comp_bench FILE j [N]           MPPC, then decompress
 *
 * "wire" is what PPP sends for the packet: flag, protocol octet, the
 * information field with its escapes for an ACCM of zero, FCS. "native"
 * is the same without compression. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppp_deflate.h"
#include "ppp_mppc.h"
#include "gen.h"

static ppp_deflate_enc_t E;
static ppp_deflate_dec_t D;
static ppp_mppc_enc_t ME;
static ppp_mppc_dec_t MD;
static uint8_t file[4 << 20];
static size_t file_n;

static size_t esc(const uint8_t *p, size_t n)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) if (p[i] == 0x7d || p[i] == 0x7e) k++;
    return k;
}

typedef struct { unsigned long in, payload, wire, native, pkts, sent_native; } tot_t;

/* alg: 0 Deflate, 1 MPPC. dec: also decompress. */
static void run(int alg, int w, long limit, const ppp_deflate_effort_t *ef, int dec, tot_t *t)
{
    static uint8_t pkt[1502], c[1502], o[1502];
    gen_t g = { 42 };
    size_t at = 0;
    long i = 0;

    memset(t, 0, sizeof *t);
    if (alg == 0) { ppp_deflate_enc_init(&E, w, ef); ppp_deflate_dec_init(&D); }
    else { ppp_mppc_enc_init(&ME); ppp_mppc_dec_init(&MD); }
    while (at < file_n && (limit == 0 || i < limit)) {
        size_t pl = file_n - at < 1460 ? file_n - at : 1460, n = 40 + pl, cn;
        gen_packet(&g, GEN_ZERO, (uint32_t)at, pkt + 2, 40);
        pkt[0] = 0; pkt[1] = 0x21;
        pkt[4] = (uint8_t)(n >> 8); pkt[5] = (uint8_t)n;
        memcpy(pkt + 42, file + at, pl);
        at += pl; i++;
        t->pkts++; t->in += n; t->payload += pl;
        t->native += 1 + 1 + n + esc(pkt + 2, n) + 2;
        if (alg == 0) {
            cn = ppp_deflate_compress(&E, pkt + 1, 1, pkt + 2, n, c, n - 3);
            if (cn) {
                t->wire += 1 + 1 + 2 + cn + esc(c, cn) + 2;
                if (dec && (ppp_deflate_decompress(&D, c, cn, o, sizeof o) != (int)n + 1 || memcmp(o, pkt + 1, n + 1))) { printf("MISMATCH\n"); exit(1); }
            } else {
                t->wire += 1 + 1 + n + esc(pkt + 2, n) + 2; t->sent_native++;
                if (dec) ppp_deflate_dec_add(&D, pkt + 1, 1, pkt + 2, n);
            }
        } else {
            unsigned bits;
            cn = ppp_mppc_compress(&ME, pkt, 2, pkt + 2, n, c, n + 1 <= 1498 ? n + 1 : 1498, &bits);
            if (cn) {
                t->wire += 1 + 1 + 2 + cn + esc(c, cn) + 2;
                if (dec) {
                    if (bits & PPP_MPPC_FLUSHED) ppp_mppc_dec_flush(&MD);
                    if (ppp_mppc_decompress(&MD, (bits & PPP_MPPC_AT_FRONT) != 0, c, cn, o, sizeof o) != (int)n + 2 || memcmp(o, pkt, n + 2)) { printf("MISMATCH\n"); exit(1); }
                }
            } else { t->wire += 1 + 1 + n + esc(pkt + 2, n) + 2; t->sent_native++; }
        }
    }
}

int main(int argc, char **argv)
{
    ppp_deflate_effort_t ef;
    tot_t t;
    FILE *f;
    int w;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: comp_bench FILE [d W [N [CHAIN [WORK [LAZY [NICE]]]]] | m [N] | i W [N] | j [N]]\n"); return 2; }
    file_n = fread(file, 1, sizeof file, f);
    fclose(f);
    ppp_deflate_effort_default(&ef);
    if (argc > 2 && (argv[2][0] == 'd' || argv[2][0] == 'i')) {
        if (argc > 5) ef.max_chain = (uint16_t)atoi(argv[5]);
        if (argc > 6) ef.work = (uint32_t)atol(argv[6]);
        if (argc > 7) ef.lazy_len = (uint16_t)atoi(argv[7]);
        if (argc > 8) ef.nice_len = (uint16_t)atoi(argv[8]);
        run(0, argc > 3 ? atoi(argv[3]) : 15, argc > 4 ? atol(argv[4]) : 0, &ef, argv[2][0] == 'i', &t);
        printf("deflate w %d chain %u work %lu lazy %u nice %u: %lu packets, %lu -> %lu wire octets (native %lu), ratio %lu.%02lu, cuts %lu\n",
               E.wbits, (unsigned)ef.max_chain, (unsigned long)ef.work, (unsigned)ef.lazy_len, (unsigned)ef.nice_len, t.pkts, t.in, t.wire, t.native,
               t.native / t.wire, t.native * 100 / t.wire % 100, (unsigned long)E.cuts);
        return 0;
    }
    if (argc > 2 && (argv[2][0] == 'm' || argv[2][0] == 'j')) {
        run(1, 0, argc > 3 ? atol(argv[3]) : 0, NULL, argv[2][0] == 'j', &t);
        printf("mppc: %lu packets, %lu -> %lu wire octets (native %lu), ratio %lu.%02lu\n", t.pkts, t.in, t.wire, t.native,
               t.native / t.wire, t.native * 100 / t.wire % 100);
        return 0;
    }
    printf("%s: %lu octets in %lu packets; native on the wire %lu octets\n", argv[1], (unsigned long)file_n, (unsigned long)((file_n + 1459) / 1460),
           (run(1, 0, 0, NULL, 1, &t), t.native));
    printf("  MPPC:         wire %7lu  ratio %lu.%02lu  file octets per wire octet %lu.%02lu  (%lu packets native)\n", t.wire,
           t.native / t.wire, t.native * 100 / t.wire % 100, t.payload / t.wire, t.payload * 100 / t.wire % 100, t.sent_native);
    for (w = PPP_DEFLATE_MIN_WBITS; w <= PPP_DEFLATE_MAX_WBITS; w++) {
        run(0, w, 0, NULL, 1, &t);
        printf("  Deflate w %2d: wire %7lu  ratio %lu.%02lu  file octets per wire octet %lu.%02lu  (%lu packets native, %lu cut short)\n", w, t.wire,
               t.native / t.wire, t.native * 100 / t.wire % 100, t.payload / t.wire, t.payload * 100 / t.wire % 100, t.sent_native, (unsigned long)E.cuts);
    }
    return 0;
}
