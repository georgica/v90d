/* See nv.h. The flash chip by physical offset: an MTD character device
 * that covers the whole chip, or a file of 16 MiB that stands for it. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <mtd/mtd-user.h>
#include "nv.h"

/* the MTD partition that is the whole chip (the device tree calls it "all") */
static int find_all(char *path, size_t cap)
{
    FILE *f = fopen("/proc/mtd", "r"); char line[128]; int n, found = -1;
    if (!f) return -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "mtd%d:", &n) == 1 && strstr(line, "\"all\"")) found = n;
    fclose(f);
    if (found < 0) return -1;
    snprintf(path, cap, "/dev/mtd%d", found);
    return 0;
}

int nf_open(nvflash *f, const char *path)
{
    struct mtd_info_user mi; struct stat st; char found[32]; const char *e;
    memset(f, 0, sizeof *f);
    f->cut = -1;
    if (!path) path = getenv("V90NV_FLASH");
    if (!path) { if (find_all(found, sizeof found)) { errno = ENODEV; return NV_E_IO; } path = found; }
    f->fd = open(path, O_RDWR);
    if (f->fd < 0) { f->fd = open(path, O_RDONLY); f->readonly = 1; }
    if (f->fd < 0) return NV_E_IO;
    if (ioctl(f->fd, MEMGETINFO, &mi) == 0) {
        f->is_mtd = 1;
        if (mi.size != NV_FLASH_SIZE || mi.erasesize != NV_SECTOR) { close(f->fd); errno = EINVAL; return NV_E_IO; }
    } else if (fstat(f->fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)NV_FLASH_SIZE) {
        close(f->fd); errno = EINVAL; return NV_E_IO;
    } else {
        if ((e = getenv("V90NV_CUT")) != 0) f->cut = atol(e);
        f->torn = getenv("V90NV_TORN") != 0;
    }
    return NV_OK;
}

void nf_close(nvflash *f) { if (f->fd >= 0) close(f->fd); f->fd = -1; }

int nf_read(nvflash *f, uint32_t off, uint8_t *buf, uint32_t n)
{
    uint32_t got = 0;
    if (off > NV_FLASH_SIZE || n > NV_FLASH_SIZE - off) return NV_E_RANGE;
    while (got < n) {
        ssize_t r = pread(f->fd, buf + got, n - got, (off_t)(off + got));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return NV_E_IO;
        got += (uint32_t)r;
    }
    return NV_OK;
}

int nf_allow(nvflash *f, uint32_t lo, uint32_t hi)
{
    if (lo >= hi || hi > NV_FLASH_SIZE || lo % NV_SECTOR || hi % NV_SECTOR || f->nallow >= sizeof f->allow / sizeof f->allow[0]) return NV_E_RANGE;
    f->allow[f->nallow].lo = lo; f->allow[f->nallow].hi = hi; f->nallow++;
    return NV_OK;
}

static int allowed(const nvflash *f, uint32_t off, uint32_t n)
{
    unsigned i;
    if (f->readonly || n == 0 || off > NV_FLASH_SIZE || n > NV_FLASH_SIZE - off) return 0;
    for (i = 0; i < f->nallow; i++) if (off >= f->allow[i].lo && off + n <= f->allow[i].hi) return 1;
    return 0;
}

static int put(nvflash *f, uint32_t off, const uint8_t *buf, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        ssize_t r = pwrite(f->fd, buf + done, n - done, (off_t)(off + done));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return NV_E_IO;
        done += (uint32_t)r;
    }
    return NV_OK;
}

/* the tests' power cut: the process ends here, in the middle of its work */
static void cut_here(nvflash *f) { fsync(f->fd); _exit(99); }

int nf_erase(nvflash *f, uint32_t off, uint32_t n)
{
    uint8_t s[NV_SECTOR]; unsigned i; int r;
    if (off % NV_SECTOR || n % NV_SECTOR || !allowed(f, off, n)) return NV_E_RANGE;
    for (; n; off += NV_SECTOR, n -= NV_SECTOR) {
        if ((r = nf_read(f, off, s, NV_SECTOR)) != NV_OK) return r;
        for (i = 0; i < NV_SECTOR && s[i] == 0xFF; i++) ;
        if (i == NV_SECTOR) continue;
        if (f->is_mtd) {
            struct erase_info_user ei;
            ei.start = off; ei.length = NV_SECTOR;
            if (ioctl(f->fd, MEMERASE, &ei)) return NV_E_IO;
        } else {
            if (f->steps == f->cut) {
                /* half an erase: some bits are back at one, nobody knows which */
                uint32_t x = off * 2654435761ul + 12345u;
                if (f->torn) {
                    for (i = 0; i < NV_SECTOR; i++) { x = x * 1664525ul + 1013904223ul; s[i] |= (uint8_t)(x >> 24); }
                    (void)put(f, off, s, NV_SECTOR);
                }
                cut_here(f);
            }
            memset(s, 0xFF, NV_SECTOR);
            if ((r = put(f, off, s, NV_SECTOR)) != NV_OK) return r;
        }
        f->steps++;
        if ((r = nf_read(f, off, s, NV_SECTOR)) != NV_OK) return r;
        for (i = 0; i < NV_SECTOR && s[i] == 0xFF; i++) ;
        if (i != NV_SECTOR) return NV_E_VERIFY;
    }
    return NV_OK;
}

int nf_program(nvflash *f, uint32_t off, const uint8_t *buf, uint32_t n)
{
    uint8_t now[NV_PAGE], back[NV_PAGE]; int r;
    if (!allowed(f, off, n)) return NV_E_RANGE;
    while (n) {
        uint32_t k = NV_PAGE - off % NV_PAGE, i;
        if (k > n) k = n;
        if ((r = nf_read(f, off, now, k)) != NV_OK) return r;
        if (memcmp(now, buf, k)) {
            if (f->is_mtd) {
                if ((r = put(f, off, buf, k)) != NV_OK) return r;
            } else {
                /* a flash chip only ever clears bits when it programs */
                if (f->steps == f->cut) {
                    if (f->torn) {
                        for (i = 0; i < (k + 1) / 2; i++) now[i] &= buf[i];
                        (void)put(f, off, now, (k + 1) / 2);
                    }
                    cut_here(f);
                }
                for (i = 0; i < k; i++) now[i] &= buf[i];
                if ((r = put(f, off, now, k)) != NV_OK) return r;
            }
            f->steps++;
            if ((r = nf_read(f, off, back, k)) != NV_OK) return r;
            if (memcmp(back, buf, k)) return NV_E_VERIFY;
        }
        off += k; buf += k; n -= k;
    }
    return NV_OK;
}
