/* Record a simulated V.34 call for rig/bench_v34: the samples the answering
 * modem receives (16-bit, host byte order = little endian on both machines
 * used), from the start of V.8 through data mode. Host only (double).
 *
 *   rec_v34 SECONDS OUT [NOISE-DB-DOWN]      mu-law line, near echo */
#include <stdio.h>
#include <stdlib.h>
#include "../rig/v34glue.h"
#include "../sim/v34peer.h"
#include "../test/line34.h"

static v34prbs a_src = { 5, 0, 0 }, a_sink = { 9, 0, 0 }, c_src = { 9, 0, 0 }, c_sink = { 5, 0, 0 };
static int  a_get(void *u) { (void)u; a_src.n++; return v34prbs_next(&a_src); }
static void a_put(void *u, int b) { (void)u; a_sink.n++; if (v34prbs_next(&a_sink) != (b & 1)) a_sink.bad++; }
static int  c_get(void *u) { (void)u; return v34prbs_next(&c_src); }
static void c_put(void *u, int b) { (void)u; c_sink.n++; if (v34prbs_next(&c_sink) != (b & 1)) c_sink.bad++; }

static v34glue_t A; static v34peer_t C; static line34_t L;
static FILE *out; static uint32_t hash = 2166136261u; static long data_at = -1;

static int16_t a_tick(void *u, int16_t rx)
{
    int16_t tx;
    (void)u;
    fwrite(&rx, sizeof rx, 1, out);
    tx = v34glue_tick(&A, rx);
    hash = (hash ^ (uint16_t)tx) * 16777619u;
    return tx;
}
static int16_t c_tick(void *u, int16_t rx) { (void)u; return v34peer_tick(&C, rx); }

int main(int argc, char **argv)
{
    line34_cfg lc; v34ans_cfg ac; v34peer_cfg pc;
    long n, total;
    if (argc < 3 || !(out = fopen(argv[2], "wb"))) { fprintf(stderr, "usage: rec_v34 SECONDS OUT [NOISE-DB-DOWN]\n"); return 2; }
    total = 8000L * atoi(argv[1]);
    memset(&lc, 0, sizeof lc);
    lc.law = 1; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.5; lc.echo_a = 0.3; lc.echo_c = 0.3;
    if (argc > 3) lc.noise = 1500.0 / pow(10, atof(argv[3]) / 20.0);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.law = 1; ac.getbit = a_get; ac.putbit = a_put;
    v34peer_default(&pc, 3000);
    pc.dx.getbit = c_get; pc.dx.putbit = c_put;
    v34glue_init(&A, &ac); v34peer_init(&C, &pc); line34_init(&L, &lc);
    for (n = 0; n < total; n++) {
        (void)line34_step(&L, a_tick, &A, c_tick, &C);
        if (data_at < 0 && A.phase == 1 && A.m.state == V34A_DATA) data_at = n;
    }
    fclose(out);
    printf("recorded %.1f s; data mode from %.2f s; %d sym/s, %d bit/s out, %d bit/s in; bits in %ld (%ld bad), far end %ld (%ld bad)\n",
           total / 8000.0, data_at / 8000.0, A.m.dx.cfg.sym_rate, A.m.dx.tx_rate, A.m.dx.rx_rate, a_sink.n, a_sink.bad, c_sink.n, c_sink.bad);
    printf("transmit hash %08x\n", (unsigned)hash);
    return 0;
}
