/* bootcount try|ok|show: the count of starts of v90d's own kernel that did
 * not come up, kept in RAM that survives a restart but not a power cut
 * (physical 0x03f01800, outside both kernels' memory; the RAM boot's
 * shim marks 0x03f01000). "try": exit 1 if two starts in a row failed,
 * else count this one and exit 0 (the launcher then jumps); "ok": the own
 * kernel is up (kinit), back to 0; "show": print it. After a power cut the
 * magic is gone and the count starts again at 0. Built into kjump
 * ("kjump --count try|ok|show"), which is static already: one C library in
 * /obi instead of two. */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define AT     0x03f01000u
#define OFF    0x800u
#define MAGIC  0x4b303956u          /* "V90K" */
#define TRIES  2u

int bootcount_main(int argc, char **argv)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32_t *p;
    uint8_t *m;
    if (argc != 2) { fprintf(stderr, "usage: kjump --count try|ok|show\n"); return 2; }
    if (fd < 0) { perror("/dev/mem"); return 2; }
    m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AT);
    if (m == MAP_FAILED) { perror("mmap"); return 2; }
    p = (volatile uint32_t *)(m + OFF);
    if (p[0] != MAGIC || p[1] > 100) { p[0] = MAGIC; p[1] = 0; }
    if (!strcmp(argv[1], "try")) {
        if (p[1] >= TRIES) return 1;
        p[1]++;
    } else if (!strcmp(argv[1], "ok")) {
        p[1] = 0;
    } else if (!strcmp(argv[1], "show")) {
        printf("%u\n", (unsigned)p[1]);
    } else {
        fprintf(stderr, "usage: kjump --count try|ok|show\n");
        return 2;
    }
    msync(m, 4096, MS_SYNC);
    return 0;
}
