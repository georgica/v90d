/* Test side only: the packet compressors behind a pipe, so that a script
 * can put an independent implementation at the other end (zlib_check.py).
 *
 *   comp_tool dc WBITS   Deflate compressor: packets in, compressed out
 *   comp_tool dd         Deflate decompressor
 *   comp_tool mc         MPPC compressor      (self-consistency runs)
 *   comp_tool md         MPPC decompressor
 *
 * Records on stdin: one octet kind, two octets length (low first), data.
 *   compressors:   kind 0 = a packet; kind 2 = reset the history
 *   decompressors: kind 0 = a compressed packet (MPPC: the first two data
 *                  octets are the header with the A/B/C bits), kind 1 = a
 *                  packet that arrived in its native form, kind 2 = reset
 * Records on stdout: the same layout; kind 0 with the result, or kind 1 and
 * no data where a compressor found the packet incompressible, or kind 3
 * where a decompressor rejects its input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppp_deflate.h"
#include "ppp_mppc.h"

static ppp_deflate_enc_t E;
static ppp_deflate_dec_t D;
static ppp_mppc_enc_t ME;
static ppp_mppc_dec_t MD;

static void emit(int kind, const uint8_t *p, size_t n)
{
    uint8_t h[3] = { (uint8_t)kind, (uint8_t)n, (uint8_t)(n >> 8) };
    fwrite(h, 1, 3, stdout);
    if (n) fwrite(p, 1, n, stdout);
}

int main(int argc, char **argv)
{
    static uint8_t in[70000], out[70000];
    uint8_t h[3];
    const char *mode = argc > 1 ? argv[1] : "";

    if (!strcmp(mode, "dc")) ppp_deflate_enc_init(&E, argc > 2 ? atoi(argv[2]) : 15, NULL);
    else if (!strcmp(mode, "dd")) ppp_deflate_dec_init(&D);
    else if (!strcmp(mode, "mc")) ppp_mppc_enc_init(&ME);
    else if (!strcmp(mode, "md")) ppp_mppc_dec_init(&MD);
    else return 2;
    while (fread(h, 1, 3, stdin) == 3) {
        size_t n = (size_t)h[1] | (size_t)h[2] << 8;
        if (n && fread(in, 1, n, stdin) != n) return 3;
        if (mode[0] == 'd' && mode[1] == 'c') {
            if (h[0] == 2) { ppp_deflate_enc_reset(&E); continue; }
            if (n == 0 || n > PPP_DEFLATE_MAX_PKT) return 4;
            n = ppp_deflate_compress(&E, in, 1, in + 1, n - 1, out, n > 3 ? n - 3 : 0);
            emit(n ? 0 : 1, out, n);
        } else if (mode[0] == 'd') {
            int r;
            if (h[0] == 2) { ppp_deflate_dec_reset(&D); continue; }
            if (h[0] == 1) { ppp_deflate_dec_add(&D, in, n, NULL, 0); continue; }
            r = ppp_deflate_decompress(&D, in, n, out, PPP_DEFLATE_MAX_PKT);
            if (r < 0) emit(3, NULL, 0); else emit(0, out, (size_t)r);
        } else if (mode[1] == 'c') {
            unsigned bits = 0;
            if (h[0] == 2) { ppp_mppc_enc_flush(&ME); continue; }
            if (n == 0 || n > PPP_MPPC_MAX_PKT) return 4;
            n = ppp_mppc_compress(&ME, in, 2, in + 2, n - 2, out + 2, n - 1, &bits);
            out[0] = (uint8_t)(bits >> 8); out[1] = 0;
            emit(n ? 0 : 1, out, n ? n + 2 : 0);
        } else {
            int r;
            if (h[0] == 2) { ppp_mppc_dec_flush(&MD); continue; }
            if (h[0] != 0 || n < 2) continue;
            if (in[0] & 0x80) ppp_mppc_dec_flush(&MD);
            r = ppp_mppc_decompress(&MD, (in[0] & 0x40) != 0, in + 2, n - 2, out, PPP_MPPC_MAX_PKT);
            if (r < 0) emit(3, NULL, 0); else emit(0, out, (size_t)r);
        }
    }
    fflush(stdout);
    return 0;
}
