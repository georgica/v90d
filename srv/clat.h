/* The callers' IPv4 over IPv6 on the box (#7): the translator of xlat.h
 * between the PPP ports and an IPv6 TUN device (CLAT_IF), whose packets
 * the kernel masquerades to the box's IPv6 address (nat_clat6). It comes up
 * once there is something to translate: a NAT64 prefix (the router's,
 * ra6.h). A caller's packet goes through it
 *   - when it is for a stand-in (198.18.0.0/15, handed out by the callers'
 *     relay for a name that has IPv6 alone), always;
 *   - otherwise only while the box has no IPv4 default route: on a network
 *     with IPv4, the callers' IPv4 goes out as it always has.
 * Daemon only. Ours; written for this project. */
#ifndef V90D_CLAT_H
#define V90D_CLAT_H
#include <stddef.h>
#include <stdint.h>
#include "xlat.h"

#define CLAT_IF "clat0"

/* [network] ipv6 and the LAN interface */
void clat_want(int on, const char *lan_if);
/* every block: coming up, the IPv4 route, packets for the callers */
void clat_poll(uint32_t now_ms);
/* a caller's IPv4 packet on port p: 1 if the translator took it (sent on,
 * answered, or dropped), 0 if it goes out as IPv4 */
int  clat_from_caller(int p, const uint8_t *pkt, size_t len);
/* the translator (its stand-ins: the callers' relay); NULL while it is down */
xlat_t *clat_xlat(void);
/* a stand-in IPv4 address for IPv6 address v6 (dnsrelay_set_standin): 0
 * while the translator is down, and for an address that is not public
 * (the box itself, the LAN): a name must not lead a caller there */
uint32_t clat_standin(const uint8_t v6[16]);
/* is it up, and does the box have IPv4 out? (the status, the log) */
int  clat_up(void);
int  clat_v4_route(void);

#endif
