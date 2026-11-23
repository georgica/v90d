/* physdump ADDR LEN: physical memory (outside the kernel's) to stdout, raw */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
int main(int c, char **v)
{
    unsigned long a, n, base;
    int fd;
    unsigned char *m;
    if (c != 3) { fprintf(stderr, "usage: physdump ADDR LEN\n"); return 2; }
    a = strtoul(v[1], 0, 16); n = strtoul(v[2], 0, 16); base = a & ~0xfffUL;
    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    m = mmap(0, n + (a - base), PROT_READ, MAP_SHARED, fd, base);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    return fwrite(m + (a - base), 1, n, stdout) == n ? 0 : 1;
}
