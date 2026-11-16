/* The kernel end of the PPP link: a TUN device carrying bare IP packets.
 * Linux only. */
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

/* IPv4 address of an interface in host byte order, 0 if it has none. */
uint32_t if_address(const char *ifname);

#endif
