/* Shared innards of the v90d daemon (srv/main.c), for the pieces that live
 * in their own translation units (satlink.c and the rest). Not a public
 * interface: it names the few things main.c owns that those pieces use.
 * Daemon only; never linked into the library or the tests' own mains. */
#ifndef V90D_DAEMON_H
#define V90D_DAEMON_H
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include "srv.h"

#define PORTS CFG_PORTS

extern srv_t       S[PORTS];        /* the per-port server objects */
extern int         n_ports;         /* how many are in use (1 or 2) */
extern const char *lan_if;          /* the network interface name */
extern int         tun_fd[PORTS];   /* each port's TUN device, or -1 */
extern char        tun_name[PORTS][16];
extern int         net_dirty;       /* set when addresses or NAT changed; netsvc_apply clears it */

/* a line in the daemon's log (levels are CFG_LOG_* from config.h) */
void        logf_(int level, const char *fmt, ...);
/* "a.b.c.d" of a host-order address into buf[0..16) */
const char *ip_str(uint32_t ip, char buf[16]);

/* In a child just forked from the daemon: an ordinary process again (no
 * real-time priority, no locked memory), so that whatever it does cannot
 * take the CPU or the memory from the line. in and out, when >= 0, become
 * its standard input and output; with close_rest every descriptor from 3
 * up is closed (for a child that runs on as itself, or a program not to be
 * given the daemon's lines and sockets). _exit(126) when a dup fails. */
static inline void child_plain(int in, int out, int close_rest)
{
    struct sched_param sp;
    int fd;
    memset(&sp, 0, sizeof sp);
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);
    (void)munlockall();
    if (in >= 0 && dup2(in, 0) < 0) _exit(126);
    if (out >= 0 && dup2(out, 1) < 0) _exit(126);
    if (close_rest) for (fd = 3; fd < 1024; fd++) (void)close(fd);
}

#endif
