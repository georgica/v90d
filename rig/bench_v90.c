/* CPU cost of the V.90 answering modem on the target, and a check that the
 * target computes exactly what the host does.
 *
 * tools/rec_v90 runs a simulated call on the host and records the codewords
 * the answering modem receives. This program replays them through the same
 * modem code (integer only, so the result must be identical: the transmit
 * hash is compared with the host's) and times it per phase of the call.
 *
 *   bench_v90 RECORDING [HOST-HASH-HEX] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../modem/answer.h"

typedef struct { uint32_t s; long n, bad; } prbs;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int get(void *u) { prbs *p = ((prbs **)u)[0]; p->n++; return nx(p); }
static void put(void *u, int b) { prbs *p = ((prbs **)u)[1]; p->n++; if (nx(p) != (b & 1)) p->bad++; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define BLK 80
enum { PH_V8, PH_P2, PH_P3, PH_DATA, NPH };
static const char *const ph_name[NPH] = { "V.8 (ANSam, CM/JM)", "Phase 2 (probing)", "Phases 3 and 4 (training)", "data mode" };

int main(int argc, char **argv)
{
    static answer_t a; static prbs src, sink; static prbs *io[2];
    static uint8_t in[BLK];
    answer_cfg ac;
    double t_ph[NPH] = { 0 }, worst[NPH] = { 0 };
    long n_ph[NPH] = { 0 }, total = 0;
    uint32_t hash = 2166136261u;
    FILE *f;
    size_t got;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: bench_v90 RECORDING [HASH]\n"); return 2; }
    src.s = 5; sink.s = 9; io[0] = &src; io[1] = &sink;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21 | ANS_MODE_V90; ac.getbit = get; ac.putbit = put; ac.user = io;
    answer_init(&a, &ac);

    while ((got = fread(in, 1, BLK, f)) == BLK) {
        int ph = a.phase == ANS_V90 && a.connected ? PH_DATA : a.phase == ANS_P3 || a.phase == ANS_V90 ? PH_P3 :
                 a.phase == ANS_P2 ? PH_P2 : PH_V8;
        double t0 = now(), dt;
        int i;
        for (i = 0; i < BLK; i++) hash = (hash ^ answer_tick_pcm(&a, in[i])) * 16777619u;
        dt = now() - t0;
        t_ph[ph] += dt; n_ph[ph] += BLK; total += BLK;
        if (dt > worst[ph]) worst[ph] = dt;
    }
    fclose(f);

    printf("replayed %.1f s; transmit hash %08x%s; connected %d, %d bit/s down, %d bit/s up; bits from line %ld (%ld bad)\n",
           total / 8000.0, (unsigned)hash,
           argc > 2 ? (strtoul(argv[2], NULL, 16) == hash ? " = host" : " DIFFERS FROM HOST") : "",
           a.connected, a.bitrate, a.rx_bitrate, sink.n, sink.bad);
    {
        int p;
        for (p = 0; p < NPH; p++) {
            if (!n_ph[p]) continue;
            printf("  %-28s %6.1f s of line: %5.1f %% of the CPU, worst 10 ms block took %5.2f ms\n",
                   ph_name[p], n_ph[p] / 8000.0, 100.0 * t_ph[p] / (n_ph[p] / 8000.0), worst[p] * 1000.0);
        }
    }
    return 0;
}
