/* The files of a release, as the stock upgrade would leave them in flash,
 * put before the loader's deciding part: does it find, check and deliver
 * exactly the system image that went in?
 *
 *   test_release CORE.img BASE.img ZIMAGE
 *
 * CORE.img and BASE.img are the partition contents (`mkgsfw.py extract` of
 * the firmware file), ZIMAGE what the box must end up running. Both sets are
 * tried; the other set holds the model's stand-in for the stock system. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "../boot.h"
#include "qspisim.h"

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb"); uint8_t *p;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    p = malloc((size_t)*n + 1);
    if (fread(p, 1, (size_t)*n, f) != (size_t)*n) { perror(path); exit(2); }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    static qspisim S; static ld_env E; static ld_result R;
    static uint8_t fdt[128];
    long ncore, nbase, nz; uint8_t *core, *base, *z, *ram, *before;
    int set;
    if (argc != 4) { fprintf(stderr, "usage: test_release CORE.img BASE.img ZIMAGE\n"); return 2; }
    core = slurp(argv[1], &ncore); base = slurp(argv[2], &nbase); z = slurp(argv[3], &nz);
    CHECK(ncore <= 0x10000);                    /* the loader ends before the state word's sector */
    CHECK(nbase <= (long)LD_BASE_SIZE);
    ram = malloc(0x400000); before = malloc(QSPI_SIZE);
    for (set = 0; set < 2; set++) {
        uint32_t c = set ? LD_CORE_B : LD_CORE_A, b = set ? LD_BASE_B : LD_BASE_A;
        qspisim_init(&S); qspisim_after_uboot(&S);
        memcpy(S.mem + LD_BOARD_OFF, "9610003725A", 12);
        memset(S.mem + c, 0xFF, 0x140000); memcpy(S.mem + c, core, (size_t)ncore);
        memset(S.mem + b, 0xFF, LD_BASE_SIZE); memcpy(S.mem + b, base, (size_t)nbase);
        memcpy(before, S.mem, QSPI_SIZE);
        memset(fdt, 0, sizeof fdt); fdt[0] = 0xD0; fdt[1] = 0x0D; fdt[2] = 0xFE; fdt[3] = 0xED; fdt[7] = sizeof fdt;
        sprintf((char *)fdt + 40, "gxp_console=0 root=/dev/mtdblock%d bootpartition=%d partgeneration=1", 6 + set, set);
        memset(&E, 0, sizeof E); qspisim_io(&S, &E.io);
        E.fdt = fdt; E.fdt_max = sizeof fdt; E.ram = ram; E.ram_max = 0x400000; memset(ram, 0xEE, 0x400000);
        ld_run(&E, &R);
        CHECK_EQ_I(R.action, LD_BOOT); CHECK_EQ_I(R.why, LD_W_OK); CHECK_EQ_I(R.set, set); CHECK_EQ_I(R.image_set, set);
        CHECK_EQ_I(R.tries, 1);
        CHECK_EQ_I(R.size, nz);                 /* not the padding the file format adds */
        CHECK(memcmp(ram, z, (size_t)nz) == 0);
        CHECK(memcmp(S.mem, before, c + LD_STATE_OFF) == 0);
        CHECK(memcmp(S.mem + c + LD_STATE_OFF + 4, before + c + LD_STATE_OFF + 4, QSPI_SIZE - (c + LD_STATE_OFF + 4)) == 0);
        CHECK_EQ_I((int)S.bad, 0);
        qspisim_free(&S);
    }
    T_REPORT("release through the loader");
}
