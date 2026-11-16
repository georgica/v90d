/* The two lists a game console's port works from (service = dreamcast):
 *   - the address list the game revival projects publish: servers that are
 *     gone, and where their stand-ins are (the text format is described in
 *     refs/dreampi-facts.md);
 *   - the ports that games want opened from the network towards the console.
 * Portable C99, no allocation. */
#ifndef V90D_SRV_DCLIST_H
#define V90D_SRV_DCLIST_H
#include <stddef.h>
#include "nat.h"

#define DCLIST_URL "https://shumania.ddns.net/dnat.txt"

/* Text of the list in, up to max rules out; returns how many. A line is
 * "old-address new-address [port [tcp|udp]]"; without a protocol it is TCP,
 * without a port every port. Lines that are anything else are left out. */
int dclist_parse(const char *text, size_t len, nat_rewrite *out, int max);
/* The port table (at most NAT_OPEN_MAX entries); returns how many. */
int dclist_ports(const nat_open **table);

#endif
