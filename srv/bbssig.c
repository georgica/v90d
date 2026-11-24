/* The signature on a fetched bulletin board list: bbssig.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bbssig.h"
#include "ed25519.h"

/* Netstalgia's keys for the list (public halves; the private ones never
 * leave its server). Two places, so a key can be replaced without a box
 * that has not been updated losing the list. */
const uint8_t bbs_sig_keys[BBS_SIG_KEYS][32] = {
#ifdef BBS_SIG_TEST_KEY
#include "test/bbskeys_test.h"
#else
#include "bbskeys.h"
#endif
};

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint64_t bbs_serial(const char *buf, size_t n)
{
    static const char tag[] = BBS_SERIAL_TAG;
    const size_t tl = sizeof tag - 1;
    uint64_t v = 0; size_t i = 0, e, d;
    int found = 0;
    while (i < n) {
        for (e = i; e < n && buf[e] != '\n'; e++) { }
        d = e; if (d > i && buf[d - 1] == '\r') d--;
        if (d - i > tl && memcmp(buf + i, tag, tl) == 0) {
            uint64_t x = 0; size_t k;
            if (found || d - i - tl > 19) return 0;
            for (k = i + tl; k < d; k++) {
                if (buf[k] < '0' || buf[k] > '9') return 0;
                x = x * 10 + (uint64_t)(buf[k] - '0');
            }
            v = x; found = 1;
        }
        i = e + 1;
    }
    return v;
}

int bbs_sig_check(const char *buf, size_t n, const uint8_t (*keys)[32], int n_keys, uint64_t *serial)
{
    static const char tag[] = BBS_SIG_TAG;
    const size_t tl = sizeof tag - 1;
    uint8_t sig[64], zero[32];
    size_t end = n, start, i;
    int k;
    if (end && buf[end - 1] == '\n') end--;
    if (end && buf[end - 1] == '\r') end--;
    start = end;
    while (start && buf[start - 1] != '\n') start--;
    if (end - start != tl + 128 || memcmp(buf + start, tag, tl)) return BBS_SIG_NONE;
    for (i = 0; i < 64; i++) {
        int h = hexval(buf[start + tl + 2 * i]), l = hexval(buf[start + tl + 2 * i + 1]);
        if (h < 0 || l < 0) return BBS_SIG_NONE;
        sig[i] = (uint8_t)(h << 4 | l);
    }
    memset(zero, 0, sizeof zero);
    if (serial) *serial = bbs_serial(buf, start);
    for (k = 0; k < n_keys; k++)
        if (memcmp(keys[k], zero, 32) && ed25519_verify(sig, (const uint8_t *)buf, start, keys[k]) == 0) return BBS_SIG_OK;
    return BBS_SIG_BAD;
}

int bbs_take(int sig_result, uint64_t got, uint64_t have)
{
    if (sig_result != BBS_SIG_OK) return BBS_TAKE_UNSIGNED;
    if (got == 0) return BBS_TAKE_OLD;      /* no serial: cannot be told from an old list */
    if (got == have) return BBS_TAKE_SAME;
    return got > have ? BBS_TAKE : BBS_TAKE_OLD;
}

static char *slurp(const char *path, size_t *n)
{
    char *buf = malloc(BBS_SIG_FILE_MAX + 1);
    ssize_t r; int fd;
    *n = 0;
    if (!buf) return NULL;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(buf); return NULL; }
    while (*n <= BBS_SIG_FILE_MAX && ((r = read(fd, buf + *n, BBS_SIG_FILE_MAX + 1 - *n)) > 0 || (r < 0 && errno == EINTR)))
        if (r > 0) *n += (size_t)r;
    close(fd);
    if (*n > BBS_SIG_FILE_MAX) { free(buf); return NULL; }
    return buf;
}

int bbs_sig_file(const char *path, const uint8_t (*keys)[32], int n_keys, uint64_t *serial)
{
    size_t n; int res;
    char *buf = slurp(path, &n);
    if (serial) *serial = 0;
    if (!buf) return BBS_SIG_NONE;
    res = bbs_sig_check(buf, n, keys, n_keys, serial);
    free(buf);
    return res;
}

uint64_t bbs_serial_file(const char *path)
{
    size_t n; uint64_t v;
    char *buf = slurp(path, &n);
    if (!buf) return 0;
    v = bbs_serial(buf, n);
    free(buf);
    return v;
}
