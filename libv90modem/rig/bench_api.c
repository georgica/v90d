/* rig/bench_v90.c through the public API: the recording of tools/rec_v90
 * replayed into libv90modem as a program that uses it would (linked against
 * the static or the shared library), in blocks of 80 samples. The transmit
 * hash must be the one bench_v90 prints for the same recording.
 *
 *   bench_api RECORDING */
#include <stdio.h>
#include <stdint.h>
#include "v90modem.h"

typedef struct { uint32_t s; long n, bad; } prbs;
static int nx(prbs *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static prbs src, sink;
static int get(void *u) { (void)u; src.n++; return nx(&src); }
static void put(void *u, int b) { (void)u; sink.n++; if (nx(&sink) != (b & 1)) sink.bad++; }

int main(int argc, char **argv)
{
    uint8_t in[80], out[80];
    uint32_t hash = 2166136261u;
    long total = 0;
    v90m_cfg c; v90m_t *m; v90m_state st;
    FILE *f;
    int i, err;

    if (argc < 2 || !(f = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: bench_api RECORDING\n"); return 2; }
    src.s = 5; sink.s = 9;
    v90m_cfg_default(&c);
    c.tx_level = 3000;
    c.modulations = V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_V90;
    c.echo_delay = 0;                   /* what bench_v90 leaves it at */
    c.getbit = get; c.putbit = put;
    if (!(m = v90m_create(&c, &err))) { fprintf(stderr, "v90m_create: %d\n", err); return 1; }
    while (fread(in, 1, sizeof in, f) == sizeof in) {
        v90m_process_g711(m, in, out, sizeof in);
        for (i = 0; i < 80; i++) hash = (hash ^ out[i]) * 16777619u;
        total += 80;
    }
    fclose(f);
    v90m_get_state(m, &st);
    printf("libv90modem %s, public API: replayed %.1f s; transmit hash %08x; connected %d, %d bit/s down, %d bit/s up; bits from line %ld (%ld bad)\n",
           v90m_version(), total / 8000.0, (unsigned)hash, st.connected, st.tx_bps, st.rx_bps, sink.n, sink.bad);
    v90m_destroy(m);
    return 0;
}
