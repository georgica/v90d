/* Stage 0c test pattern layout, shared by generator, simulator and analyser.
 * All positions are in 8 kHz samples from the start of the pattern file. */
#ifndef V90D_PAT_H
#define V90D_PAT_H
#include <stdint.h>

#define PAT_LEAD      4000            /* silence */
#define PAT_SYNC      PAT_LEAD        /* 1 kHz square at Ucode 100 */
#define PAT_SYNC_LEN  2000
#define PAT_MW        (PAT_SYNC + PAT_SYNC_LEN + 2000)   /* digital milliwatt */
#define PAT_MW_LEN    8000
#define PAT_STAIR     (PAT_MW + PAT_MW_LEN + 800)        /* 128 steps, Ucode 127 down to 0 */
#define PAT_STEP_LEN  400
#define PAT_IMP       (PAT_STAIR + 128 * PAT_STEP_LEN + 800) /* impulse train */
#define PAT_IMP_PERIOD 80
#define PAT_IMP_COUNT 2000            /* 20 s */
#define PAT_IMP_UCODE 110
#define PAT_PRBS      (PAT_IMP + PAT_IMP_PERIOD * PAT_IMP_COUNT + 800)
#define PAT_PRBS_LEN  80000           /* 10 s of random codewords */
#define PAT_PRBS_MAXU 120             /* Ucodes 0..119, random sign */
#define PAT_TOTAL     (PAT_PRBS + PAT_PRBS_LEN + 4000)

static const uint8_t pat_milliwatt[8] = { 0x1E, 0x0B, 0x0B, 0x1E, 0x9E, 0x8B, 0x8B, 0x9E };

static inline uint32_t pat_lcg(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}
#define PAT_PRBS_SEED 0x5eed1234u
#endif
