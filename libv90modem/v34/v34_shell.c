/* v90d/v34 - shell mapper (V.34 9.4) and its inverse. The mapper follows
 * the algorithm of the Recommendation step by step; the inverse is the same
 * arithmetic run backwards and needs no division. */
#include "v34_priv.h"

static inline uint32_t G2(const v34_params_t *p, int i)
{
    return (i < 0 || i > 2 * (p->map.M - 1)) ? 0 : p->g2[i];
}

static inline uint32_t G4(const v34_params_t *p, int i)
{
    return (i < 0 || i > 4 * (p->map.M - 1)) ? 0 : p->g4[i];
}

/* (9-17) to (9-24): split a pair sum s into two ring indices given the
 * index e of the pair among the g2(s) pairs with that sum */
static void split(int M, int s, int e, uint8_t *m0, uint8_t *m1)
{
    if (s < M) {
        *m0 = (uint8_t)e;
        *m1 = (uint8_t)(s - e);
    } else {
        *m1 = (uint8_t)(M - 1 - e);
        *m0 = (uint8_t)(s - *m1);
    }
}

void v34_shell_map(const v34_params_t *p, uint32_t r0, uint8_t ring[8])
{
    int M = p->map.M, A, B, C, D, lim;
    uint32_t r1, r2, r3, r4, r5, t, E, F, G, H;

    /* 2) largest A with z8(A) <= R0 */
    for (A = 0; A < 8 * (M - 1) && p->z8[A + 1] <= r0; A++)
        ;
    /* 3) largest B with R1 >= 0 */
    r1 = r0 - p->z8[A];
    lim = A < 4 * (M - 1) ? A : 4 * (M - 1);
    for (B = 0; B < lim; B++) {
        t = G4(p, B) * G4(p, A - B);
        if (r1 < t) break;
        r1 -= t;
    }
    /* 4) */
    t = G4(p, B);
    r2 = r1 % t;
    r3 = r1 / t;
    /* 5.1) largest C with R4 >= 0 */
    r4 = r2;
    lim = B < 2 * (M - 1) ? B : 2 * (M - 1);
    for (C = 0; C < lim; C++) {
        t = G2(p, C) * G2(p, B - C);
        if (r4 < t) break;
        r4 -= t;
    }
    /* 5.2) largest D with R5 >= 0 */
    r5 = r3;
    lim = (A - B) < 2 * (M - 1) ? (A - B) : 2 * (M - 1);
    for (D = 0; D < lim; D++) {
        t = G2(p, D) * G2(p, A - B - D);
        if (r5 < t) break;
        r5 -= t;
    }
    /* 6.1), 6.2) */
    t = G2(p, C); E = r4 % t; F = r4 / t;
    t = G2(p, D); G = r5 % t; H = r5 / t;

    split(M, C, (int)E, &ring[0], &ring[1]);
    split(M, B - C, (int)F, &ring[2], &ring[3]);
    split(M, D, (int)G, &ring[4], &ring[5]);
    split(M, A - B - D, (int)H, &ring[6], &ring[7]);
}

/* index of the pair (m0, m1) among the pairs with the same sum */
static inline uint32_t unsplit(int M, int m0, int m1)
{
    return (uint32_t)((m0 + m1 < M) ? m0 : M - 1 - m1);
}

uint32_t v34_shell_unmap(const v34_params_t *p, const uint8_t ring[8])
{
    int M = p->map.M, A, B, C, D, i;
    uint32_t E, F, G, H, r0, r1, r2, r3;
    uint8_t m[8];

    for (i = 0; i < 8; i++)             /* tolerate garbage after errors */
        m[i] = (uint8_t)(ring[i] < M ? ring[i] : M - 1);

    C = m[0] + m[1];
    B = C + m[2] + m[3];
    D = m[4] + m[5];
    A = B + D + m[6] + m[7];
    E = unsplit(M, m[0], m[1]);
    F = unsplit(M, m[2], m[3]);
    G = unsplit(M, m[4], m[5]);
    H = unsplit(M, m[6], m[7]);

    r2 = F * G2(p, C) + E;                      /* R4, then R2 by (9-11) */
    for (i = 0; i < C; i++)
        r2 += G2(p, i) * G2(p, B - i);
    r3 = H * G2(p, D) + G;                      /* R5, then R3 by (9-12) */
    for (i = 0; i < D; i++)
        r3 += G2(p, i) * G2(p, A - B - i);
    r1 = r3 * G4(p, B) + r2;                    /* (9-9), (9-10) */
    r0 = r1 + p->z8[A];                         /* (9-8) */
    for (i = 0; i < B; i++)
        r0 += G4(p, i) * G4(p, A - i);
    return r0;
}
