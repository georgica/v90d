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

int bbs_sig_check(const char *buf, size_t n, const uint8_t (*keys)[32], int n_keys)
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
    for (k = 0; k < n_keys; k++)
        if (memcmp(keys[k], zero, 32) && ed25519_verify(sig, (const uint8_t *)buf, start, keys[k]) == 0) return BBS_SIG_OK;
    return BBS_SIG_BAD;
}

int bbs_sig_file(const char *path, const uint8_t (*keys)[32], int n_keys)
{
    char *buf = malloc(BBS_SIG_FILE_MAX + 1);
    size_t n = 0; ssize_t r; int fd, res;
    if (!buf) return BBS_SIG_NONE;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { free(buf); return BBS_SIG_NONE; }
    while (n <= BBS_SIG_FILE_MAX && ((r = read(fd, buf + n, BBS_SIG_FILE_MAX + 1 - n)) > 0 || (r < 0 && errno == EINTR)))
        if (r > 0) n += (size_t)r;
    close(fd);
    res = n > BBS_SIG_FILE_MAX ? BBS_SIG_NONE : bbs_sig_check(buf, n, keys, n_keys);
    free(buf);
    return res;
}
