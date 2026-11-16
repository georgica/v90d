/* Stage 0d: fixed-point DSP kernel benchmark. Portable C99, no float in the
 * measured kernels, no allocation. Build for x86 and ARMv5TE from this file.
 *
 * Kernels (the three inner loops a V.34 receiver + echo canceller live in):
 *   rfir   real FIR,  Q15 taps x Q15 data, 32-bit accumulate (echo canceller filter)
 *   rlms   real LMS tap update                                  (echo canceller adapt)
 *   cfir   complex FIR, Q15, 32-bit accumulate                  (fractionally spaced equaliser)
 *   clms   complex LMS tap update                               (equaliser adapt)
 * Output: million multiply-accumulates per second (MMAC/s) per kernel, then a
 * load estimate for the receiver budget model at the bottom of this file.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NTAPS 128
#define ITERS 20000

static int16_t rd[NTAPS], rc[NTAPS];
static int16_t cdr[NTAPS], cdi[NTAPS], ccr[NTAPS], cci[NTAPS];
static volatile int32_t sink;

static uint32_t lcg = 12345u;
static int16_t rnd16(void)
{
    lcg = lcg * 1664525u + 1013904223u;
    return (int16_t)(lcg >> 17) - 16384;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static int32_t rfir(const int16_t *d, const int16_t *c, int n)
{
    int32_t acc = 0;
    int i;
    for (i = 0; i < n; i++)
        acc += ((int32_t)d[i] * c[i]) >> 6; /* guard bits, no 64-bit math */
    return acc;
}

static void rlms(const int16_t *d, int16_t *c, int n, int16_t err_mu)
{
    int i;
    for (i = 0; i < n; i++)
        c[i] = (int16_t)(c[i] + (((int32_t)d[i] * err_mu) >> 15));
}

static void cfir(const int16_t *dr, const int16_t *di, const int16_t *cr,
                 const int16_t *ci, int n, int32_t *outr, int32_t *outi)
{
    int32_t ar = 0, ai = 0;
    int i;
    for (i = 0; i < n; i++) {
        ar += ((int32_t)dr[i] * cr[i] - (int32_t)di[i] * ci[i]) >> 7;
        ai += ((int32_t)dr[i] * ci[i] + (int32_t)di[i] * cr[i]) >> 7;
    }
    *outr = ar;
    *outi = ai;
}

static void clms(const int16_t *dr, const int16_t *di, int16_t *cr, int16_t *ci,
                 int n, int16_t er, int16_t ei)
{
    int i;
    for (i = 0; i < n; i++) { /* c += mu * err * conj(d) */
        cr[i] = (int16_t)(cr[i] + (((int32_t)er * dr[i] + (int32_t)ei * di[i]) >> 15));
        ci[i] = (int16_t)(ci[i] + (((int32_t)ei * dr[i] - (int32_t)er * di[i]) >> 15));
    }
}

static unsigned mmac_x100(uint64_t macs, uint64_t us)
{
    if (us == 0)
        us = 1;
    return (unsigned)(macs * 100u / us); /* MAC/us == MMAC/s, x100 */
}

int main(void)
{
    uint64_t t0, t1;
    unsigned r1, r2, r3, r4;
    int32_t a = 0, b = 0;
    int i, k;

    for (i = 0; i < NTAPS; i++) {
        rd[i] = rnd16(); rc[i] = rnd16() >> 3;
        cdr[i] = rnd16(); cdi[i] = rnd16();
        ccr[i] = rnd16() >> 3; cci[i] = rnd16() >> 3;
    }

    t0 = now_us();
    for (k = 0; k < ITERS; k++) { rd[k % NTAPS] ^= 1; a += rfir(rd, rc, NTAPS); }
    t1 = now_us(); sink = a;
    r1 = mmac_x100((uint64_t)ITERS * NTAPS, t1 - t0);

    t0 = now_us();
    for (k = 0; k < ITERS; k++) rlms(rd, rc, NTAPS, (int16_t)(k & 63));
    t1 = now_us(); sink = rc[5];
    r2 = mmac_x100((uint64_t)ITERS * NTAPS, t1 - t0);

    t0 = now_us();
    for (k = 0; k < ITERS; k++) { cdr[k % NTAPS] ^= 1; cfir(cdr, cdi, ccr, cci, NTAPS, &a, &b); sink = a + b; }
    t1 = now_us();
    r3 = mmac_x100((uint64_t)ITERS * NTAPS * 4, t1 - t0);

    t0 = now_us();
    for (k = 0; k < ITERS; k++) clms(cdr, cdi, ccr, cci, NTAPS, (int16_t)(k & 31), (int16_t)(k & 15));
    t1 = now_us(); sink = ccr[5];
    r4 = mmac_x100((uint64_t)ITERS * NTAPS * 4, t1 - t0);

    printf("rfir %u.%02u MMAC/s\n", r1 / 100, r1 % 100);
    printf("rlms %u.%02u MMAC/s\n", r2 / 100, r2 % 100);
    printf("cfir %u.%02u MMAC/s\n", r3 / 100, r3 % 100);
    printf("clms %u.%02u MMAC/s\n", r4 / 100, r4 % 100);

    /* Budget model, MAC per second, V.34 rx at 3200 sym/s + EC at 8 kHz:
     *   echo canceller: 128 real taps, filter + adapt, per 8 kHz sample
     *   rx front end:   48 real taps x2 (I/Q demod + band filter), per 8 kHz sample
     *   equaliser:      64 complex taps T/2, 1 output + 1 adapt per symbol
     *   trellis etc.:   allowance equal to 30% of the above
     */
    {
        uint64_t ec = (uint64_t)8000 * 128, fe = (uint64_t)8000 * 96;
        uint64_t eq = (uint64_t)3200 * 64 * 4;
        /* percent CPU = MAC/s / (MMAC/s * 1e6) * 100 */
        uint64_t pct_x100 = 0;
        if (r1 && r2 && r3 && r4)
            pct_x100 = ec * 10000 / ((uint64_t)r1 * 10000) + ec * 10000 / ((uint64_t)r2 * 10000)
                     + fe * 10000 / ((uint64_t)r1 * 10000)
                     + eq * 10000 / ((uint64_t)r3 * 10000) + eq * 10000 / ((uint64_t)r4 * 10000);
        pct_x100 = pct_x100 * 130 / 100;
        printf("budget: EC %lu + FE %lu + EQ 2x%lu MAC/s -> est. load %lu.%02lu %% of one core\n",
               (unsigned long)ec * 2, (unsigned long)fe, (unsigned long)eq,
               (unsigned long)(pct_x100 / 100), (unsigned long)(pct_x100 % 100));
    }
    return 0;
}
