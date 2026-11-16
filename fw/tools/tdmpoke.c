/* v90d bring-up tool: look at the SoC's TDM block from user space through
 * /dev/mem. User space cannot keep a 4 ms FIFO fed, so this takes single
 * snapshots: one FIFO-full of transmit samples and what was received while
 * they went out. For finding out where a sample sits in a FIFO word.
 *
 *   tdmpoke setup            reset and configure the block (4 slots of 16
 *                            bits, we are clock master) and start it
 *   tdmpoke regs             the registers
 *   tdmpoke snap [AMPL [SHIFT [SLOT]]]
 *                            send 32 frames of a 1 kHz tone of AMPL (16-bit
 *                            scale, default 0: silence) shifted left by
 *                            SHIFT (default 16) in SLOT (default 0), and
 *                            print the frames received meanwhile
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>

enum { T_ENABLE = 0x00, T_CONFIG1 = 0x04, T_CONFIG2 = 0x08, T_FSYNC = 0x10, T_FSYNC_DUR = 0x14, T_FIFO_WM = 0x18,
       T_INT_EN = 0x20, T_INT_CLR = 0x28, T_INT_STATUS = 0x2C, T_STATUS = 0x30, T_TX = 0x38, T_RX = 0x3C, T_FDA = 0x1800 };
static volatile uint8_t *tdm, *cmu;
static uint32_t rd(unsigned o) { return *(volatile uint32_t *)(tdm + o); }
static void wr(unsigned o, uint32_t v) { *(volatile uint32_t *)(tdm + o) = v; }
static uint32_t crd(unsigned o) { return *(volatile uint32_t *)(cmu + o); }
static void cwr(unsigned o, uint32_t v) { *(volatile uint32_t *)(cmu + o) = v; }
static void spin_us(long us)
{
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    do clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < us);
}

int main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC), i;
    if (fd < 0) { perror("/dev/mem"); return 1; }
    tdm = mmap(0, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x05F00000);
    cmu = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x05300000);
    if (tdm == MAP_FAILED || cmu == MAP_FAILED) { perror("mmap"); return 1; }
    if (argc < 2) { fprintf(stderr, "usage: tdmpoke setup | regs | snap [AMPL [SHIFT [SLOT]]]\n"); return 2; }

    if (!strcmp(argv[1], "setup")) {
        wr(T_ENABLE, 0);
        cwr(0x58, crd(0x58) & ~(1u << 9)); spin_us(20);         /* clock gated while it is configured */
        cwr(0x50, crd(0x50) | (1u << 9)); spin_us(2);
        cwr(0x50, crd(0x50) & ~(1u << 9));
        for (i = 0; i < 4; i++) wr(T_FDA + 4 * (unsigned)i, ((unsigned)i << 8) | 0xC0 | 16);
        wr(T_CONFIG1, 0x1A03); wr(T_CONFIG2, 0x0301); wr(T_FSYNC, 0); wr(T_FSYNC_DUR, 0);
        wr(T_FIFO_WM, 0x0707); wr(T_INT_EN, 0);
        cwr(0x58, crd(0x58) | (1u << 9));
        for (i = 0; i < 128; i++) { wr(T_TX, 0); (void)rd(T_RX); }
        wr(T_ENABLE, 1);
        printf("TDM block configured and started\n");
    } else if (!strcmp(argv[1], "regs")) {
        static const unsigned o[] = { 0x00, 0x04, 0x08, 0x10, 0x14, 0x18, 0x20, 0x2C, 0x30 };
        for (i = 0; i < 9; i++) printf("tdm %02x = %08x\n", o[i], rd(o[i]));
        for (i = 0; i < 4; i++) printf("fda %d  = %08x\n", i, rd(T_FDA + 4 * (unsigned)i));
        printf("cmu clksw %08x  clkdiv4 %08x  pll3 %08x  reset1 %08x  gate1 %08x\n", crd(0x1C), crd(0x2C), crd(0x08), crd(0x50), crd(0x58));
    } else if (!strcmp(argv[1], "snap")) {
        static const int tone[8] = { 0, 7071, 10000, 7071, 0, -7071, -10000, -7071 };
        int ampl = argc > 2 ? atoi(argv[2]) : 0, shift = argc > 3 ? atoi(argv[3]) : 16, slot = argc > 4 ? atoi(argv[4]) : 0;
        static uint32_t rx[128]; uint32_t st0, st1;
        for (i = 0; i < 128; i++) (void)rd(T_RX);               /* what was waiting */
        wr(T_INT_CLR, 0xFFFFF);
        for (i = 0; i < 128; i++) {
            int s = (i & 3) == slot ? tone[(i >> 2) & 7] * ampl / 10000 : 0;
            wr(T_TX, (uint32_t)s << shift);
        }
        st0 = rd(T_INT_STATUS);
        spin_us(3800);
        for (i = 0; i < 120; i++) rx[i] = rd(T_RX);
        st1 = rd(T_INT_STATUS);
        printf("status %08x -> %08x\n", st0, st1);
        for (i = 0; i < 30; i++) printf("%2d: %08x %08x %08x %08x\n", i, rx[4 * i], rx[4 * i + 1], rx[4 * i + 2], rx[4 * i + 3]);
    } else if (!strcmp(argv[1], "snapw") && argc > 3) {
        /* a 1 kHz square of two raw FIFO words in slot 0 */
        uint32_t w0 = (uint32_t)strtoul(argv[2], 0, 16), w1 = (uint32_t)strtoul(argv[3], 0, 16);
        static uint32_t rx[128];
        for (i = 0; i < 128; i++) (void)rd(T_RX);
        wr(T_INT_CLR, 0xFFFFF);
        for (i = 0; i < 128; i++) wr(T_TX, (i & 3) ? 0 : ((i >> 2) & 4) ? w1 : w0);
        spin_us(3800);
        for (i = 0; i < 120; i++) rx[i] = rd(T_RX);
        for (i = 0; i < 30; i++) printf("%08x ", rx[4 * i]);
        printf("\n");
    } else return 2;
    return 0;
}
