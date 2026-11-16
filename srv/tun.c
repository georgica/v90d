/* See tun.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if_tun.h>
#include "tun.h"

static void set_addr(struct sockaddr *sa, uint32_t ip)
{
    struct sockaddr_in *in = (struct sockaddr_in *)(void *)sa;
    memset(in, 0, sizeof *in);
    in->sin_family = AF_INET; in->sin_addr.s_addr = htonl(ip);
}

int tun_open(const char *name, uint32_t local, uint32_t peer, int mtu)
{
    struct ifreq ifr;
    int fd, s = -1, err;
    fd = open("/dev/net/tun", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) goto fail;
    s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) goto fail;
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
    return fd;
fail:
    err = errno;
    if (s >= 0) close(s);
    close(fd);
    errno = err;
    return -1;
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
