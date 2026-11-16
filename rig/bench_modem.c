/* CPU cost of the real modem code on the target: two V.22bis modems back to
 * back (so each sample runs 2 transmitters + 2 receivers), and the V.8 answer
 * phase. Prints the share of one core a single modem needs in real time. */
#include <stdio.h>
#include <time.h>
#include "../modem/v22bis.h"
#include "../modem/v8ans.h"

static unsigned l = 1;
static int gb(void *u) { (void)u; l = l * 1664525u + 1013904223u; return (l >> 24) & 1; }
static unsigned long nbits;
static void pb(void *u, int b) { (void)u; nbits += (unsigned)b + 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void)
{
    static v22bis_t a, c; static v8ans_t v8;
    v8_menu_t local = { V8_CALLF_DATA, V8_MOD_V22 | V8_MOD_V21, 3, 0, 0, 0, 0, 0, 0 };
    int16_t atx = 0, ctx = 0, t;
    const int secs = 30;
    double t0, t1;
    int n;

    v22m_init(&a, V22_ANSWERER, 1, 3000, gb, pb, 0);
    v22m_init(&c, V22_CALLER, 1, 3000, gb, pb, 0);
    t0 = now();
    for (n = 0; n < secs * 8000; n++) { t = v22m_tick(&a, ctx); ctx = v22m_tick(&c, atx); atx = t; }
    t1 = now();
    printf("V.22bis duplex: %d s of line time for 2 modems in %.3f s -> %.2f %% of one core per modem "
           "(rate %d, ready %d%d, mse %d)\n", secs, t1 - t0, 100.0 * (t1 - t0) / (2.0 * secs),
           a.bitrate, a.tx_ready, a.rx_ready, (int)a.rx.mse);

    v8ans_init(&v8, 8000, 4000, &local);
    t0 = now();
    for (n = 0; n < 5 * 8000; n++) v8ans_tick(&v8, (int16_t)(n & 63), &t);
    t1 = now();
    printf("V.8 answer (ANSam + V.21 receive): 5 s in %.3f s -> %.2f %% of one core\n",
           t1 - t0, 100.0 * (t1 - t0) / 5.0);
    return 0;
}
