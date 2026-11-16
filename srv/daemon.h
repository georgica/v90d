/* Shared innards of the v90d daemon (srv/main.c), for the pieces that live
 * in their own translation units (satlink.c and the rest). Not a public
 * interface: it names the few things main.c owns that those pieces use.
 * Daemon only; never linked into the library or the tests' own mains. */
#ifndef V90D_DAEMON_H
#define V90D_DAEMON_H
#include <stdint.h>
#include "srv.h"

#define PORTS CFG_PORTS

extern srv_t       S[PORTS];        /* the per-port server objects */
extern int         n_ports;         /* how many are in use (1 or 2) */
extern const char *lan_if;          /* the network interface name */

/* a line in the daemon's log (levels are CFG_LOG_* from config.h) */
void        logf_(int level, const char *fmt, ...);
/* "a.b.c.d" of a host-order address into buf[0..16) */
const char *ip_str(uint32_t ip, char buf[16]);

#endif
