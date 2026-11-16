/* See line.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "line.h"

int line_open(line_t *l, const char *spec)
{
    l->fd = -1; l->dev = 0;
    if (strncmp(spec, "fd:", 3) == 0) {
        char *end; long n = strtol(spec + 3, &end, 10);
        if (*end || end == spec + 3 || n < 0 || n > 1 << 20) { errno = EINVAL; return -1; }
        l->fd = (int)n;
        return 0;
    }
    if (strncmp(spec, "unix:", 5) == 0) {
        struct sockaddr_un sa;
        int fd, err;
        if (strlen(spec + 5) >= sizeof sa.sun_path) { errno = ENAMETOOLONG; return -1; }
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return -1;
        memset(&sa, 0, sizeof sa); sa.sun_family = AF_UNIX; strcpy(sa.sun_path, spec + 5);
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { err = errno; close(fd); errno = err; return -1; }
        l->fd = fd;
        return 0;
    }
    if (strncmp(spec, "dev:", 4) == 0) {
        int fd = open(spec + 4, O_RDWR | O_CLOEXEC);
        if (fd < 0) return -1;
        l->fd = fd; l->dev = 1;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int all(int fd, uint8_t *buf, size_t n, int wr)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = wr ? write(fd, buf + done, n - done) : read(fd, buf + done, n - done);
        if (r == 0 && !wr) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        done += (size_t)r;
    }
    return 1;
}

int line_exchange(line_t *l, const uint8_t tx[LINE_BLOCK], int have_tx, uint8_t rx[LINE_BLOCK], int *offhook)
{
    uint8_t in[1 + LINE_BLOCK], out[LINE_BLOCK];
    int r;
    if (have_tx) {
        memcpy(out, tx, LINE_BLOCK);
        r = all(l->fd, out, LINE_BLOCK, 1);
        if (r < 0) return errno == EPIPE ? 0 : -1;
    }
    r = all(l->fd, in, sizeof in, 0);
    if (r <= 0) return r;
    *offhook = in[0] != 0;
    memcpy(rx, in + 1, LINE_BLOCK);
    return 1;
}

int16_t line_lin_sample(int16_t codec)
{
    long v = codec, a = v < 0 ? -v : v;
    a = (a * 7 + 1) / 2;
    if (a > 32767) a = 32767;
    return (int16_t)(v < 0 ? -a : a);
}

int line_read2(line_t *l, uint8_t rx[LINE_BLOCK], int16_t lin[LINE_BLOCK], int *linear, int *offhook)
{
    uint8_t in[1 + 2 * LINE_BLOCK];
    int i;
    *linear = 0;
    if (!l->dev) {
        int r = all(l->fd, in, 1 + LINE_BLOCK, 0);
        if (r <= 0) return r;
    } else {
        /* the driver hands out one whole block per read, in the format its
         * codec is in: the length says which */
        ssize_t r;
        do r = read(l->fd, in, sizeof in); while (r < 0 && errno == EINTR);
        if (r <= 0) return (int)r;
        if (r == 1 + 2 * LINE_BLOCK) *linear = 1;
        else if (r != 1 + LINE_BLOCK) { errno = EPROTO; return -1; }
    }
    *offhook = in[0] != 0;
    if (*linear) for (i = 0; i < LINE_BLOCK; i++) lin[i] = line_lin_sample((int16_t)(uint16_t)(in[1 + 2 * i] | (in[2 + 2 * i] << 8)));
    else memcpy(rx, in + 1, LINE_BLOCK);
    return 1;
}

int line_read(line_t *l, uint8_t rx[LINE_BLOCK], int *offhook)
{
    int16_t lin[LINE_BLOCK];
    int linear, r = line_read2(l, rx, lin, &linear, offhook);
    if (r == 1 && linear) { errno = EPROTO; return -1; }
    return r;
}

int line_write(line_t *l, const uint8_t tx[LINE_BLOCK])
{
    uint8_t out[LINE_BLOCK];
    memcpy(out, tx, LINE_BLOCK);
    if (all(l->fd, out, LINE_BLOCK, 1) < 0) return errno == EPIPE ? 0 : -1;
    return 0;
}

int line_set_law(line_t *l, int alaw)
{
    uint8_t c = alaw ? 'a' : 'u';
    if (!l->dev) return 0;
    return write(l->fd, &c, 1) == 1 ? 0 : -1;
}

int line_set_linear(line_t *l, int on)
{
    uint8_t c = on ? 'l' : 'c';
    if (!l->dev) { if (!on) return 0; errno = ENOTSUP; return -1; }
    return write(l->fd, &c, 1) == 1 ? 0 : -1;
}

void line_close(line_t *l) { if (l->fd >= 0) close(l->fd); l->fd = -1; }
