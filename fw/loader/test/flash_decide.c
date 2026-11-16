/* What the loader decides on a whole flash image in a file: the tests of
 * the running system's flash tool (fw/tools/test_v90nv_boot.py) use it to
 * ask "and what would the box do at its next start".
 *
 *   flash_decide [-w] FLASH.bin SET
 *
 * SET is the set the stock boot loader started us from (0, 1; -1 = it did
 * not say). Prints one line: BOOT or RESET, the reason number, set, the set
 * the image came from, attempts counted, size and CRC-32 of what would be
 * started. With -w what the loader wrote to the flash goes back to the file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../boot.h"
#include "qspisim.h"

int main(int argc, char **argv)
{
    static qspisim S; static ld_env E; static ld_result R;
    static uint8_t fdt[128];
    int wr = argc == 4 && !strcmp(argv[1], "-w"), set;
    FILE *f; uint8_t *ram;
    if (argc != 3 + wr) { fprintf(stderr, "usage: flash_decide [-w] FLASH.bin SET\n"); return 2; }
    set = atoi(argv[2 + wr]);
    qspisim_init(&S); qspisim_after_uboot(&S);
    f = fopen(argv[1 + wr], "rb");
    if (!f || fread(S.mem, 1, QSPI_SIZE, f) != QSPI_SIZE) { fprintf(stderr, "flash_decide: %s is not a 16 MiB flash image\n", argv[1 + wr]); return 2; }
    fclose(f);
    ram = malloc(0x400000);
    memset(&E, 0, sizeof E); qspisim_io(&S, &E.io);
    if (set >= 0) {
        fdt[0] = 0xD0; fdt[1] = 0x0D; fdt[2] = 0xFE; fdt[3] = 0xED; fdt[7] = sizeof fdt;
        sprintf((char *)fdt + 40, "gxp_console=0 root=/dev/mtdblock%d bootpartition=%d partgeneration=1", 6 + set, set);
        E.fdt = fdt; E.fdt_max = sizeof fdt;
    }
    E.ram = ram; E.ram_max = 0x400000;
    ld_run(&E, &R);
    printf("%s %d %d %d %d %lu %08lx\n", R.action == LD_BOOT ? "BOOT" : "RESET", R.why, R.set, R.image_set, R.tries,
           (unsigned long)R.size, (unsigned long)(R.action == LD_BOOT ? ld_crc32(0, ram, R.size) : 0));
    if (S.bad) { fprintf(stderr, "flash_decide: the driver did something the chip model does not know\n"); return 3; }
    if (wr) {
        f = fopen(argv[2], "r+b");
        if (!f || fwrite(S.mem, 1, QSPI_SIZE, f) != QSPI_SIZE || fclose(f)) { perror(argv[2]); return 2; }
    }
    return 0;
}
