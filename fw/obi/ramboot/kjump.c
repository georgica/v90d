/* kjump PAYLOAD STUB [R1 [R2]]: boot PAYLOAD from RAM on the OBi200's stock
 * 2.6.30 kernel, which has no kexec. The payload goes to physical 0x03800000
 * (outside Linux's memory); STUB (stub.S) is written after the LED module's
 * code and led_ioctl is patched to branch to it; then an ioctl on /dev/led
 * runs it in kernel mode: interrupts off, caches flushed, identity map,
 * caches and MMU off, jump to the payload with r1, r2. Never returns.
 * The addresses are checked against /proc/kallsyms first. A PAYLOAD in
 * the .xz format (mkpayload.py) is unpacked here first, with the Linux
 * kernel's own XZ decoder (lib/xz), while the stock kernel still feeds the
 * watchdog. */
#define _FILE_OFFSET_BITS 64
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "xz.h"

#define LOAD_PHYS 0x03800000u
#define LOAD_MAX  0x00600000u   /* 6 MiB: up to the RAM log at 0x03e00000 */
#define STUB_AT   0xbf019c00u

static uint32_t sym(const char *name)
{
    char line[256], n[128], t; unsigned long a; FILE *f = fopen("/proc/kallsyms", "r");
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "%lx %c %127s", &a, &t, n) == 3 && !strcmp(n, name)) { fclose(f); return (uint32_t)a; }
    if (f) fclose(f);
    fprintf(stderr, "no symbol %s\n", name); exit(1);
}

static size_t slurp(const char *p, uint8_t *b, size_t max)
{
    FILE *f = fopen(p, "rb"); size_t n;
    if (!f) { perror(p); exit(1); }
    n = fread(b, 1, max, f); fclose(f); return n;
}

/* the payload: as it is, or unpacked from .xz (its CRC32 checked) */
static size_t payload(const char *p, uint8_t *out, size_t max)
{
    static const uint8_t magic[6] = { 0xfd, '7', 'z', 'X', 'Z', 0 };
    uint8_t *in = malloc(max);
    size_t n;
    struct xz_buf b;
    struct xz_dec *d;
    enum xz_ret r;
    if (!in) { perror("malloc"); exit(1); }
    n = slurp(p, in, max);
    if (n == max) { fprintf(stderr, "payload over %zu octets\n", max); exit(1); }
    if (n < sizeof magic || memcmp(in, magic, sizeof magic)) { memcpy(out, in, n); free(in); return n; }
    xz_crc32_init();
    d = xz_dec_init(XZ_SINGLE, 0);
    if (!d) { fprintf(stderr, "xz: no memory\n"); exit(1); }
    b.in = in; b.in_pos = 0; b.in_size = n;
    b.out = out; b.out_pos = 0; b.out_size = max;
    r = xz_dec_run(d, &b);
    xz_dec_end(d);
    free(in);
    if (r != XZ_STREAM_END) { fprintf(stderr, "xz: the payload does not unpack (%d)\n", (int)r); exit(1); }
    printf("payload unpacked: %zu octets from %zu\n", b.out_pos, n);
    return b.out_pos;
}

int main(int argc, char **argv)
{
    static uint8_t pay[LOAD_MAX], stub[512], evict[2 << 20];
    size_t np, ns, i;
    uint32_t ioc, br, *sw;
    int mem, km, led;
    volatile uint8_t *dst;
    if (argc < 3) { fprintf(stderr, "usage: kjump PAYLOAD STUB [R1 [R2]]\n"); return 2; }
    np = payload(argv[1], pay, sizeof pay);
    ns = slurp(argv[2], stub, sizeof stub);
    /* the stub's literals: check them against this kernel, set r1, r2 */
    sw = (uint32_t *)(stub + ns - 7 * 4);
    if (sw[0] != sym("arm926_flush_kern_cache_all") || sw[1] != sym("setup_mm_for_reboot") ||
        sw[2] != sym("cpu_arm926_proc_fin") || sw[3] != sym("cpu_arm926_reset") || sw[4] != LOAD_PHYS) {
        fprintf(stderr, "the stub was built for another kernel\n"); return 1;
    }
    sw[5] = argc > 3 ? strtoul(argv[3], 0, 16) : 0;
    sw[6] = argc > 4 ? strtoul(argv[4], 0, 16) : 0;
    ioc = sym("led_ioctl");
    /* the payload, uncached */
    mem = open("/dev/mem", O_RDWR | O_SYNC);
    dst = mmap(0, LOAD_MAX, PROT_READ | PROT_WRITE, MAP_SHARED, mem, LOAD_PHYS);
    if (dst == MAP_FAILED) { perror("mmap"); return 1; }
    for (i = 0; i < np; i++) dst[i] = pay[i];
    for (i = 0; i < np; i++) if (dst[i] != pay[i]) { fprintf(stderr, "payload did not stick at %zu\n", i); return 1; }
    /* the stub after the module's code, then the branch into it */
    km = open("/dev/kmem", O_RDWR);
    if (km < 0 || lseek(km, (off_t)STUB_AT, SEEK_SET) == (off_t)-1 || write(km, stub, ns) != (ssize_t)ns) { perror("kmem stub"); return 1; }
    br = 0xEA000000u | (((STUB_AT - (ioc + 8)) >> 2) & 0x00ffffffu);
    printf("payload %zu octets at %08x, stub %zu at %08x, led_ioctl %08x -> b (%08x), r1 %08x r2 %08x\n",
           np, LOAD_PHYS, ns, STUB_AT, ioc, br, sw[5], sw[6]);
    fflush(stdout);
    sync();
    if (lseek(km, (off_t)ioc, SEEK_SET) == (off_t)-1 || write(km, &br, 4) != 4) { perror("kmem patch"); return 1; }
    /* the writes went through the data cache: push them out to RAM */
    for (i = 0; i < sizeof evict; i += 32) evict[i] = (uint8_t)i;
    for (i = 0; i < sizeof evict; i += 32) evict[i]++;
    led = open("/dev/led", O_RDONLY);
    ioctl(led, 0, evict);
    fprintf(stderr, "still here: the jump did not happen\n");
    return 1;
}
