/* Stage 0c: write the mu-law test pattern (see pat.h) to stdout or a file. */
#include <stdio.h>
#include <string.h>
#include "g711.h"
#include "pat.h"

static uint8_t buf[PAT_TOTAL];

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "wb") : stdout;
    uint32_t seed = PAT_PRBS_SEED;
    int i, s;

    if (!f) { perror(argv[1]); return 1; }
    memset(buf, 0xFF, sizeof(buf));
    for (i = 0; i < PAT_SYNC_LEN; i++)
        buf[PAT_SYNC + i] = ucode_to_ulaw(100, (i & 7) < 4);
    for (i = 0; i < PAT_MW_LEN; i++)
        buf[PAT_MW + i] = pat_milliwatt[i & 7];
    for (s = 0; s < 128; s++)
        for (i = 0; i < PAT_STEP_LEN; i++)
            buf[PAT_STAIR + s * PAT_STEP_LEN + i] = ucode_to_ulaw((unsigned)(127 - s), (i & 7) < 4);
    for (i = 0; i < PAT_IMP_COUNT; i++)
        buf[PAT_IMP + i * PAT_IMP_PERIOD] = ucode_to_ulaw(PAT_IMP_UCODE, 1);
    for (i = 0; i < PAT_PRBS_LEN; i++) {
        uint32_t r = pat_lcg(&seed);
        buf[PAT_PRBS + i] = ucode_to_ulaw((r >> 1) % PAT_PRBS_MAXU, (int)(r & 1u));
    }
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) { perror("write"); return 1; }
    fprintf(stderr, "pattern: %d samples (%d.%d s)\n", PAT_TOTAL, PAT_TOTAL / 8000, PAT_TOTAL % 8000 / 800);
    return 0;
}
