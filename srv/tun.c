/* See tun.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/route.h>
#include <linux/if_tun.h>
#include <linux/ppp_defs.h>
#include <linux/ppp-ioctl.h>
#include "ppp_hdlc.h"
#include "tun.h"

#ifndef N_PPP
#define N_PPP 3
#endif

/* A device made the PPP way (no TUN driver): the kernel's PPP unit pppN,
 * with an asynchronous PPP channel on a pseudo-terminal under it. We hold
 * the terminal's master end and speak HDLC-framed PPP with the kernel's
 * framer on the other side; it carries bare IP (protocol 0x0021) and
 * nothing else, no LCP: the kernel's PPP needs none to pass packets. */
typedef struct {
    int master, slave, unit, chan;      /* master is the descriptor the caller holds; -1 = free */
    ppp_hdlc_rx_t rx;
    uint8_t in[2048];
    int in_n, in_pos;
} pppdev;

#define PPPDEV_MAX 4
static pppdev P[PPPDEV_MAX] = { { -1, -1, -1, -1, { 0 }, { 0 }, 0, 0 }, { -1, -1, -1, -1, { 0 }, { 0 }, 0, 0 },
                                { -1, -1, -1, -1, { 0 }, { 0 }, 0, 0 }, { -1, -1, -1, -1, { 0 }, { 0 }, 0, 0 } };

static pppdev *ppp_of(int fd)
{
    int i;
    if (fd < 0) return NULL;
    for (i = 0; i < PPPDEV_MAX; i++) if (P[i].master == fd) return &P[i];
    return NULL;
}

static void set_addr(struct sockaddr *sa, uint32_t ip)
{
    struct sockaddr_in *in = (struct sockaddr_in *)(void *)sa;
    memset(in, 0, sizeof *in);
    in->sin_family = AF_INET; in->sin_addr.s_addr = htonl(ip);
}

/* address, peer, MTU and up, on whichever device `name` is */
static int if_setup(const char *name, uint32_t local, uint32_t peer, int mtu)
{
    struct ifreq ifr;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), err;
    if (s < 0) return -1;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    set_addr(&ifr.ifr_addr, local);
    if (ioctl(s, SIOCSIFADDR, &ifr) < 0) goto fail;
    set_addr(&ifr.ifr_dstaddr, peer);
    if (ioctl(s, SIOCSIFDSTADDR, &ifr) < 0) goto fail;
    set_addr(&ifr.ifr_netmask, 0xFFFFFFFFu);
    if (ioctl(s, SIOCSIFNETMASK, &ifr) < 0) goto fail;
    ifr.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, &ifr) < 0) goto fail;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) goto fail;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_POINTOPOINT;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) goto fail;
    close(s);
    return 0;
fail:
    err = errno;
    close(s);
    errno = err;
    return -1;
}

static void ppp_free(pppdev *d)
{
    if (d->chan >= 0) close(d->chan);
    if (d->unit >= 0) close(d->unit);
    if (d->slave >= 0) close(d->slave);
    if (d->master >= 0) close(d->master);
    d->master = d->slave = d->unit = d->chan = -1;
}

/* pppN on a pseudo-terminal; name must be "ppp" and a number */
static int ppp_open(const char *name, uint32_t local, uint32_t peer, int mtu)
{
    pppdev *d = NULL;
    struct termios t;
    struct npioctl np;
    int i, ldisc = N_PPP, unit, chan, err;
    char *end;
    if (strncmp(name, "ppp", 3) != 0) { errno = EINVAL; return -1; }
    unit = (int)strtol(name + 3, &end, 10);
    if (*end || end == name + 3) { errno = EINVAL; return -1; }
    for (i = 0; i < PPPDEV_MAX; i++) if (P[i].master < 0) { d = &P[i]; break; }
    if (!d) { errno = EMFILE; return -1; }
    memset(d, 0, sizeof *d);
    d->master = d->slave = d->unit = d->chan = -1;

    d->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (d->master < 0 || grantpt(d->master) < 0 || unlockpt(d->master) < 0) goto fail;
    (void)fcntl(d->master, F_SETFD, FD_CLOEXEC);     /* posix_openpt takes no O_CLOEXEC: not for the children */
    d->slave = open(ptsname(d->master), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (d->slave < 0) goto fail;
    if (tcgetattr(d->slave, &t) == 0) { cfmakeraw(&t); tcsetattr(d->slave, TCSANOW, &t); }
    if (ioctl(d->slave, TIOCSETD, &ldisc) < 0) goto fail;
    if (ioctl(d->slave, PPPIOCGCHAN, &chan) < 0) goto fail;
    d->unit = open("/dev/ppp", O_RDWR | O_CLOEXEC);
    if (d->unit < 0 || ioctl(d->unit, PPPIOCNEWUNIT, &unit) < 0) goto fail;
    d->chan = open("/dev/ppp", O_RDWR | O_CLOEXEC);
    if (d->chan < 0 || ioctl(d->chan, PPPIOCATTCHAN, &chan) < 0 || ioctl(d->chan, PPPIOCCONNECT, &unit) < 0) goto fail;
    np.protocol = PPP_IP; np.mode = NPMODE_PASS;
    if (ioctl(d->unit, PPPIOCSNPMODE, &np) < 0) goto fail;
    fcntl(d->master, F_SETFD, FD_CLOEXEC);
    fcntl(d->master, F_SETFL, fcntl(d->master, F_GETFL) | O_NONBLOCK);
    if (if_setup(name, local, peer, mtu) < 0) goto fail;
    ppp_hdlc_rx_init(&d->rx, PPP_HDLC_BUF);
    d->rx.rx_accm = 0;      /* take control characters as they come */
    return d->master;
fail:
    err = errno;
    ppp_free(d);
    errno = err;
    return -1;
}

int tun_readdress(const char *name, uint32_t local, uint32_t peer, int mtu)
{
    return if_setup(name, local, peer, mtu);
}

int tun_open(const char *name, uint32_t local, uint32_t peer, int mtu)
{
    struct ifreq ifr;
    int fd, err;
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        /* no TUN driver (the OBi200's Linux 2.6.30): the PPP way */
        if ((errno == ENOENT || errno == ENODEV || errno == ENXIO) && access("/dev/ppp", F_OK) == 0)
            return ppp_open(name, local, peer, mtu);
        return -1;
    }
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0 || if_setup(name, local, peer, mtu) < 0) {
        err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

int tun_open6(const char *name, int mtu, const uint8_t prefix[16], int plen)
{
    struct ifreq ifr;
    struct in6_rtmsg rt;
    int fd, s, err;
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) goto fail;
    s = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) goto fail;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    if (ioctl(s, SIOCSIFMTU, &ifr) < 0) goto fail_s;
    if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0) goto fail_s;
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(s, SIOCSIFFLAGS, &ifr) < 0) goto fail_s;
    memset(&rt, 0, sizeof rt);
    memcpy(&rt.rtmsg_dst, prefix, 16);
    rt.rtmsg_dst_len = (unsigned short)plen;
    rt.rtmsg_metric = 1;
    rt.rtmsg_flags = RTF_UP;
    rt.rtmsg_ifindex = (int)if_nametoindex(name);
    if (ioctl(s, SIOCADDRT, &rt) < 0 && errno != EEXIST) goto fail_s;
    close(s);
    return fd;
fail_s:
    err = errno; close(s); errno = err;
fail:
    err = errno; close(fd); errno = err;
    return -1;
}

ssize_t tun_read(int fd, uint8_t *buf, size_t cap)
{
    pppdev *d = ppp_of(fd);
    if (!d) return read(fd, buf, cap);
    for (;;) {
        while (d->in_pos < d->in_n) {
            const uint8_t *f;
            size_t n, h = 0;
            if (!ppp_hdlc_rx_byte(&d->rx, d->in[d->in_pos++])) continue;
            f = d->rx.buf; n = d->rx.len;
            if (n >= 2 && f[0] == 0xff && f[1] == 0x03) h = 2;     /* address and control */
            if (n < h + 1) continue;
            if (f[h] & 1) {                                         /* one-octet protocol */
                if (f[h] != 0x21) continue;
                h += 1;
            } else {
                if (n < h + 2 || f[h] != 0x00 || f[h + 1] != 0x21) continue;
                h += 2;
            }
            if (n - h > cap) continue;
            memcpy(buf, f + h, n - h);
            return (ssize_t)(n - h);
        }
        {
            ssize_t r = read(fd, d->in, sizeof d->in);
            if (r <= 0) return r;
            d->in_n = (int)r; d->in_pos = 0;
        }
    }
}

ssize_t tun_write(int fd, const uint8_t *pkt, size_t len)
{
    static const uint8_t head[4] = { 0xff, 0x03, 0x00, 0x21 };
    uint8_t out[2 * PPP_HDLC_BUF + 8];
    size_t n, done = 0;
    int tries = 0;
    if (!ppp_of(fd)) return write(fd, pkt, len);
    n = ppp_hdlc_encode(out, sizeof out, head, sizeof head, pkt, len, 0xffffffffu, 1);
    if (!n) { errno = EMSGSIZE; return -1; }
    while (done < n) {
        ssize_t r = write(fd, out + done, n - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            /* the terminal's buffer is full: the kernel's side is behind.
             * Half a frame would corrupt the next; finish this one. */
            if (errno == EAGAIN && done && ++tries < 50) { usleep(200); continue; }
            return -1;
        }
        done += (size_t)r;
    }
    return (ssize_t)len;
}

void tun_close(int fd)
{
    pppdev *d = ppp_of(fd);
    if (d) ppp_free(d);
    else if (fd >= 0) close(fd);
}

uint32_t if_address(const char *ifname)
{
    struct ifreq ifr;
    uint32_t ip = 0;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return 0;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0)
        ip = ntohl(((struct sockaddr_in *)(void *)&ifr.ifr_addr)->sin_addr.s_addr);
    close(s);
    return ip;
}
