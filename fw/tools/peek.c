/* v90d bring-up tool: read or write SoC registers through /dev/mem.
 *   peek ADDR [COUNT]        32-bit words, COUNT of them (default 1)
 *   peek -h ADDR [COUNT]     16-bit
 *   peek -w ADDR VALUE       write 32-bit;  -W: write 16-bit
 *   peek -l ADDR             copy standard input into memory from ADDR on
 *   peek -s ADDR LEN         sum of LEN octets from ADDR (to check a load)
 *   peek -r ADDR LEN         LEN octets from ADDR to standard output
 * Addresses and values in hex. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    int half = 0, wr = 0, a = 1, fd, n = 1, i;
    unsigned long addr, val = 0, page, off;
    volatile uint8_t *m;
    if (argc > 2 && (!strcmp(argv[1], "-l") || !strcmp(argv[1], "-s") || !strcmp(argv[1], "-r"))) {
        unsigned long at = strtoul(argv[2], 0, 16), total = 0, sum = 0, want = argc > 3 ? strtoul(argv[3], 0, 0) : 0;
        int load = argv[1][1] == 'l';
        static unsigned char buf[4096];
        fd = open("/dev/mem", (load ? O_RDWR : O_RDONLY) | O_SYNC);
        if (fd < 0) { perror("/dev/mem"); return 1; }
        if (at & 0xFFF) { fprintf(stderr, "peek: the address must be page aligned\n"); return 2; }
        for (;;) {
            size_t got = 0; ssize_t r = 0; size_t k;
            if (load) {
                while (got < sizeof buf && (r = read(0, buf + got, sizeof buf - got)) > 0) got += (size_t)r;
                if (r < 0) { perror("read"); return 1; }
            } else got = want - total < sizeof buf ? want - total : sizeof buf;
            if (!got) break;
            m = mmap(0, 0x1000, load ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, (off_t)(at + total));
            if (m == MAP_FAILED) { perror("mmap"); return 1; }
            if (load) memcpy((void *)m, buf, got);
            for (k = 0; k < got; k++) sum += m[k];
            if (argv[1][1] == 'r' && write(1, (const void *)m, got) != (ssize_t)got) { perror("write"); return 1; }
            munmap((void *)m, 0x1000);
            total += got;
            if (got < sizeof buf) break;
        }
        if (argv[1][1] != 'r') printf("%lu octets at %08lx, sum %08lx\n", total, at, sum & 0xFFFFFFFFul);
        return 0;
    }
    if (argc > 1 && argv[1][0] == '-') {
        half = argv[1][1] == 'h' || argv[1][1] == 'W';
        wr = argv[1][1] == 'w' || argv[1][1] == 'W';
        a = 2;
    }
    if (argc < a + 1 + wr) { fprintf(stderr, "usage: peek [-h|-w|-W] ADDR [COUNT|VALUE]\n"); return 2; }
    addr = strtoul(argv[a], 0, 16);
    if (wr) val = strtoul(argv[a + 1], 0, 16);
    else if (argc > a + 1) n = atoi(argv[a + 1]);
    page = addr & ~0xFFFul; off = addr & 0xFFFul;
    fd = open("/dev/mem", (wr ? O_RDWR : O_RDONLY) | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    m = mmap(0, 0x2000, wr ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, (off_t)page);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    if (wr) {
        if (half) *(volatile uint16_t *)(m + off) = (uint16_t)val;
        else *(volatile uint32_t *)(m + off) = (uint32_t)val;
        return 0;
    }
    for (i = 0; i < n; i++) {
        unsigned long o = off + (unsigned long)i * (half ? 2 : 4);
        if (half) printf("%08lx: %04x\n", addr + (unsigned long)i * 2, *(volatile uint16_t *)(m + o));
        else printf("%08lx: %08x\n", addr + (unsigned long)i * 4, *(volatile uint32_t *)(m + o));
    }
    return 0;
}
