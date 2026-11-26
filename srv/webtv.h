/* WebTV Redialed, the default WebTV server ([services] webtv_server): a
 * WebTV / MSN TV box, and WebTV for Dreamcast, talk to fixed addresses of
 * their old network, which the daemon sends to the WebTV server (nat.h).
 * WebTV Redialed's address is looked up by name (they asked that it be
 * followed; CFG_WTV_REDIALED_ADDR only stands in until a lookup has
 * worked), and only a public IPv4 answer is taken. */
#ifndef V90D_SRV_WEBTV_H
#define V90D_SRV_WEBTV_H
#include <stdint.h>
#include <netdb.h>

#define WEBTV_HOST     "community.webtv.zone"
#define WEBTV_EVERY_MS 600000u          /* looked up again every 10 minutes */

/* 1 when the address (host order) is a public IPv4 unicast address: not
 * private, shared, loopback, link-local, documentation, benchmarking,
 * multicast or reserved */
int      webtv_public(uint32_t a);
/* the first public IPv4 address among the answers (host order), 0 = none */
uint32_t webtv_pick(const struct addrinfo *ai);

#endif
