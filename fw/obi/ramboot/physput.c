/* physput ADDR: stdin (up to 60 KiB) to physical memory at ADDR as a note
 * the next boot can read back: u32 magic "NOTE", u32 length, the text. */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define ROOM 0xf000u
int main(int c, char **v)
{
    static char buf[ROOM];
    unsigned long a;
    unsigned n = 0;
    ssize_t r;
    int fd;
    unsigned char *m;
    if (c != 2) { fprintf(stderr, "usage: physput ADDR < text\n"); return 2; }
    a = strtoul(v[1], 0, 16);
    while (n < ROOM && (r = read(0, buf + n, ROOM - n)) > 0) n += r;
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    m = mmap(0, ROOM + 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    memcpy(m, "NOTE", 4); memcpy(m + 4, &n, 4); memcpy(m + 8, buf, n);
    msync(m, ROOM + 8, MS_SYNC);
    return 0;
}
