/* tdmoff: stop the audio block's DMA rings and both phone channels' TDM
 * (and hold the SLIC in reset), so nothing writes into RAM once a kernel is
 * RAM-booted. obiline starts the TDM and never stops it, and stopping the
 * TDM alone leaves the DMA rings running (the receive ring writes into
 * 0x03180000..). The rings go off first, while the TDM clock still runs:
 * the ring command is acknowledged (bit 31 clears) on the audio clock. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#define FXS_TDM_OFF 0x718bu
#define FXS_RESET   0x71b6u
#define RING_CMD 0x80c           /* in the 4 KiB page at 0x8001b000 */
static int rings_off(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC), n;
    volatile unsigned int *g;
    if (fd < 0) { perror("/dev/mem"); return 1; }
    g = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x8001b000);
    if (g == MAP_FAILED) { perror("mmap"); return 1; }
    printf("ring command %08x, valid %08x\n", g[RING_CMD / 4], g[0x804 / 4]);
    g[RING_CMD / 4] = 0x83100000u;
    for (n = 0; n < 1000 && (g[RING_CMD / 4] & 0x80000000u); n++) usleep(1000);
    printf("rings off: %08x after %d ms%s\n", g[RING_CMD / 4], n, n == 1000 ? " (NOT acknowledged)" : "");
    return n == 1000;
}
int main(void)
{
    const char *dev[] = { "/dev/fxs0", "/dev/fxs1" };
    int i, bad = rings_off();
    for (i = 0; i < 2; i++) {
        int fd = open(dev[i], O_RDWR);
        if (fd < 0) { perror(dev[i]); bad = 1; continue; }
        if (ioctl(fd, FXS_TDM_OFF, 0) < 0) { perror("TDM off"); bad = 1; }
        if (i == 0 && ioctl(fd, FXS_RESET, 0) < 0) { perror("SLIC reset"); bad = 1; }
        close(fd);
    }
    return bad;
}
