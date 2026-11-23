/* The kernel end of the PPP link: a TUN device carrying bare IP packets,
 * or the kernel's own PPP where there is no TUN driver. Linux only. */
#ifndef V90D_SRV_TUN_H
#define V90D_SRV_TUN_H
#include <stddef.h>
#include <stdint.h>

/* Create the device `name` (for example "ppp0"), give it our address with
 * the peer's as point-to-point destination (host byte order), the MTU, and
 * bring it up. The kernel then routes the peer's address into it. Returns a
 * non-blocking descriptor: read() yields one IP packet for the caller,
 * write() takes one from the caller. -1 with errno on failure. */
int tun_open(const char *name, uint32_t local, uint32_t peer, int mtu);
/* Without a TUN driver (/dev/net/tun missing: the OBi200's Linux 2.6.30)
 * tun_open makes the device the PPP way: the kernel's PPP unit pppN over an
 * asynchronous PPP channel on a pseudo-terminal, `name` then being "ppp"
 * and the unit number. The descriptor is then a terminal carrying HDLC
 * frames: read, write and close it with these, which do the framing (and
 * pass a TUN descriptor straight through). */
#include <sys/types.h>
ssize_t tun_read(int fd, uint8_t *buf, size_t cap);
ssize_t tun_write(int fd, const uint8_t *pkt, size_t len);
void    tun_close(int fd);

/* IPv4 address of an interface in host byte order, 0 if it has none. */
uint32_t if_address(const char *ifname);

#endif
