/* CPU cost of the answering modem.
 *
 * The modem is integer only, so its behaviour is the same on the host and on
 * the ARM target bit for bit. `record` (host) runs a whole call against the
 * calling peer and writes what the answerer received, sample by sample, with
 * a checksum of what it sent; `count` (ARM, under qemu, see `make cost-arm`)
 * replays the first n of those samples into the answerer alone, so that qemu
 * counts the modem's instructions and nothing else. The cost per sample in a
 * stretch of the call is the difference between two values of n inside it.
 *
 *   test_bench record <rate 0..4> <file>     prints TRN= RXT= DATA= END= (sample numbers)
 *   test_bench count <file> <n>              replay n samples, quietly
 *   test_bench check <file>                  replay all, compare the checksum
 *   test_bench fir <n>                       n calls of the 128-tap reference FIR
 *
 * The reference FIR is the kernel of stage0/bench.c, which the project timed
 * on the real box (25.4 MMAC/s on the 400 MHz ARM926): instructions per
 * sample / instructions per MAC gives the cost in MACs, and that against
 * 25.4e6 / 8000 an estimate of the share of the CPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "v32.h"

static v32_t m;
static uint32_t lfsr = 1;
static int  getbit(void *u) { (void)u; lfsr = lfsr * 1103515245u + 12345u; return (lfsr >> 16) & 1; }
static unsigned long sunk;
static void putbit(void *u, int b) { (void)u; sunk += (unsigned)b; }

#define NTAPS 128
static int16_t rd[NTAPS], rc[NTAPS];
static volatile int32_t vsink;
static int32_t rfir(const int16_t *d, const int16_t *c, int n)
{
    int32_t acc = 0;
    int i;
    for (i = 0; i < n; i++)
        acc += ((int32_t)d[i] * c[i]) >> 6;
    return acc;
}

static void modem_init(int rate)
{
    v32_cfg c;
    memset(&c, 0, sizeof c);
    c.level = 3000; c.rates = 1u << rate; c.getbit = getbit; c.putbit = putbit; c.user = 0; c.echo_delay = 0;
    lfsr = 1;
    v32_init(&m, &c);
}

#define MAXN (8000L * 12)
static int16_t rxbuf[MAXN];

static int replay(const char *file, long n, int check)
{
    FILE *f = fopen(file, "rb");
    int32_t hdr[4];
    uint32_t sum = 0;
    long i, have;
    if (!f) { fprintf(stderr, "cannot open %s\n", file); return 2; }
    if (fread(hdr, sizeof hdr[0], 4, f) != 4) { fclose(f); return 2; }
    have = (long)fread(rxbuf, sizeof rxbuf[0], (size_t)hdr[1], f);
    fclose(f);
    if (have != hdr[1]) return 2;
    if (n < 0 || n > have) n = have;
    modem_init(hdr[0]);
    for (i = 0; i < n; i++) {
        int16_t tx = v32_tick(&m, rxbuf[i]);
        sum = sum * 31u + (uint16_t)tx;
    }
    if (check) {
        int ok = sum == (uint32_t)hdr[2] && m.connected && m.bitrate == hdr[3];
        printf("  replay of %ld samples: transmit checksum %08lx (recorded %08lx), %s at %d bit/s: %s\n", n,
               (unsigned long)sum, (unsigned long)(uint32_t)hdr[2], m.connected ? "connected" : "NOT connected", m.bitrate,
               ok ? "same as the host run" : "DIFFERENT");
        return ok ? 0 : 1;
    }
    return 0;
}

#ifndef __arm__
#include "calltest.h"
static call_t k;
static int record(int rate, const char *file)
{
    v32line_cfg lc;
    v32_cfg ac;
    int32_t hdr[4];
    uint32_t sum = 0;
    long n = 0, t_trn = 0, t_rxt = 0, t_data = 0, end;
    int16_t a_rx = 0;
    FILE *f;

    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = -60;
    call_init(&k, &lc, 1u << rate, 1u << rate, 3000, 3000);
    /* the answerer exactly as the replay will build it */
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.rates = 1u << rate; ac.getbit = getbit; ac.putbit = putbit;
    lfsr = 1;
    v32_init(&k.m, &ac);
    end = MAXN;
    while (n < end) {
        int16_t tx;
        rxbuf[n] = a_rx;
        tx = v32_tick(&k.m, a_rx);
        sum = sum * 31u + (uint16_t)tx;
        a_rx = v32line_tick(&k.line, tx, call_peer, &k);
        n++;
        if (!t_trn && k.m.state == V32S_TRN1) t_trn = n;
        if (!t_rxt && k.m.state == V32S_RXTRAIN && k.m.rx.mode == V32_RX_TRAIN) t_rxt = n;
        if (!t_data && k.m.connected) { t_data = n; end = n + 8000L * 3 < MAXN ? n + 8000L * 3 : MAXN; }
        if (k.m.state == V32S_FAILED) break;
    }
    if (!t_data) { fprintf(stderr, "no connection (state %d)\n", k.m.state); return 1; }
    f = fopen(file, "wb");
    if (!f) return 2;
    hdr[0] = rate; hdr[1] = (int32_t)n; hdr[2] = (int32_t)sum; hdr[3] = k.m.bitrate;
    fwrite(hdr, sizeof hdr[0], 4, f);
    fwrite(rxbuf, sizeof rxbuf[0], (size_t)n, f);
    fclose(f);
    printf("TRN=%ld RXT=%ld DATA=%ld END=%ld\n", t_trn, t_rxt, t_data, n);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "count")) return replay(argv[2], atol(argv[3]), 0);
    if (argc >= 3 && !strcmp(argv[1], "check")) return replay(argv[2], -1, 1);
    if (argc >= 3 && !strcmp(argv[1], "fir")) {
        long i, n = atol(argv[2]);
        int32_t a = 0;
        for (i = 0; i < NTAPS; i++) { rd[i] = (int16_t)(i * 37 - 2000); rc[i] = (int16_t)(1000 - i * 13); }
        for (i = 0; i < n; i++) { rd[i & (NTAPS - 1)] ^= 1; a += rfir(rd, rc, NTAPS); }
        vsink = a;
        return 0;
    }
#ifndef __arm__
    if (argc >= 4 && !strcmp(argv[1], "record")) return record(atoi(argv[2]), argv[3]);
#endif
    fprintf(stderr, "usage: test_bench record <rate> <file> | count <file> <n> | check <file> | fir <n>\n");
    return 2;
}
