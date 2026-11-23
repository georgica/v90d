/* fxstest: bring-up check of the telephony driver through the stock ABI
 * that obiline uses: release the SLIC from reset, read its ID (register 0)
 * on both channels, then read and write /dev/pcm for 2 s and count. */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#define FXS_RD    0xc00171b2u
#define FXS_PREP  0x7191u
#define FXS_ON    0x718au
#define FXS_OFF   0x718bu
#define FXS_RESET 0x71b6u
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(void)
{
    int fx[2], pcm, i, rd = 0, wr = 0, nz = 0;
    unsigned int a[2];
    unsigned char buf[320];
    double t0;
    for (i = 0; i < 2; i++) {
        char n[16]; snprintf(n, sizeof n, "/dev/fxs%d", i);
        if ((fx[i] = open(n, O_RDWR)) < 0) { perror(n); return 1; }
    }
    if (ioctl(fx[0], FXS_RESET, 0) < 0) perror("reset hold");
    usleep(250000);
    if (ioctl(fx[0], FXS_RESET, 1) < 0) perror("reset release");
    usleep(250000);
    for (i = 0; i < 2; i++) {
        a[0] = 0; a[1] = 0;
        if (ioctl(fx[i], FXS_RD, a) < 0) perror("reg read");
        printf("fxs%d register 0 = 0x%02x\n", i, a[1] & 0xff);
        a[0] = 1; a[1] = 0; ioctl(fx[i], FXS_RD, a);
        printf("fxs%d register 1 = 0x%02x\n", i, a[1] & 0xff);
    }
    for (i = 0; i < 2; i++) {
        if (ioctl(fx[i], FXS_PREP, 0) < 0) perror("prep");
        if (ioctl(fx[i], FXS_ON, 0) < 0) perror("tdm on");
    }
    if ((pcm = open("/dev/pcm", O_RDWR)) < 0) { perror("/dev/pcm"); return 1; }
    memset(buf, 0, sizeof buf);
    t0 = now();
    while (now() - t0 < 2.0) {
        struct pollfd p = { pcm, POLLIN | POLLOUT, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        if (p.revents & POLLIN) {
            if (read(pcm, buf, sizeof buf) == (ssize_t)sizeof buf) {
                rd++;
                for (i = 0; i < 320; i++) if (buf[i]) { nz++; break; }
            }
        }
        if (p.revents & POLLOUT) {
            memset(buf, 0, sizeof buf);
            if (write(pcm, buf, sizeof buf) == (ssize_t)sizeof buf) wr++;
        }
    }
    printf("pcm: %d reads (%d with nonzero samples), %d writes in 2 s\n", rd, nz, wr);
    for (i = 0; i < 2; i++) ioctl(fx[i], FXS_OFF, 0);
    return 0;
}
