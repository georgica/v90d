/* v90d/cfg - the OpenVPN profile for the tunnel to the IWANGO games' network
 * (service = dreamcast, dreamcast_iwango = on). The owner of the box gets
 * the profile from whoever runs that network and gives it to the Dreamcast
 * page; the daemon hands it to the OpenVPN program in the image.
 *
 * A profile is a program for something that runs as root, so only what a
 * client's profile needs is let through: the directives listed in vpn.c,
 * and certificates and keys as inline blocks. Anything that names a file,
 * runs a script, loads a plugin, writes a log or changes where all the
 * traffic goes is refused, with the line it stands on; a route may only
 * lead into 10.0.0.0/8 (the daemon holds the server's pushed routes to the
 * same). Pure C99. */
#ifndef V90D_CFG_VPN_H
#define V90D_CFG_VPN_H
#include <stddef.h>

#define VPN_PROFILE_MAX 12288   /* octets */
#define VPN_MSG_MAX     120

/* 0 = acceptable; -1 = not, and msg (cap >= VPN_MSG_MAX) says why */
int vpn_profile_check(const char *text, size_t len, char *msg, size_t cap);
/* "host port" of the first remote line into out (cut to fit); 0, or -1 when
 * there is none */
int vpn_profile_remote(const char *text, size_t len, char *out, size_t cap);

#endif
