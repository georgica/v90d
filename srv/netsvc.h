/* The daemon's network layer (srv/netsvc.c): the per-port services'
 * background work and the assembly of the TUN devices, the nftables plan,
 * proxy ARP and the archive gateway from the current settings. main.c calls
 * it; it reaches back for the shared symbols in daemon.h. Daemon only. */
#ifndef V90D_NETSVC_H
#define V90D_NETSVC_H
#include <stddef.h>
#include <stdint.h>

/* install the TUN devices, NAT, proxy ARP and the gateway for the settings
 * now in force (net_dirty is cleared); call when the lines are idle */
void netsvc_apply(void);
/* the per-service background work, once per line block */
void netsvc_poll(uint32_t now_ms);
/* on the way out: stop the gateway and the VPN, drop the NAT and proxy ARP,
 * give the router's ports back */
void netsvc_stop(void);

/* adopt()/net_apply share these: the console's current lease address for a
 * port (0 = none), and the configured peer address a port should have */
uint32_t netsvc_lan_ip(int port);
void     netsvc_set_peer_conf(int port, uint32_t ppp_peer);

/* the VPN profile posted on the Consoles page (an httpd keep callback) */
int  netsvc_on_vpn(void *ctx, const char *profile, size_t len);
/* "host port" of the stored VPN profile's server into out; 0, or -1 (none) */
int  netsvc_vpn_remote(char *out, size_t cap);

/* program and file overrides from the command line */
void netsvc_set_arch(const char *path);     /* -A: the archive gateway ("" = none) */
void netsvc_set_dclist(const char *cmd);    /* -D: fetch the console address list ("" = none) */
void netsvc_set_dcnow(const char *prog);    /* -G: run instead of wget for Dreamcast Now */
void netsvc_set_lease(const char *cmd);     /* -E: print the console's lease (instead of udhcpc) */
void netsvc_set_vpnprog(const char *prog);  /* -O: the OpenVPN program ("" = none) */
void netsvc_set_vpnpath(const char *path);  /* -V: where the VPN profile is kept */

#endif
