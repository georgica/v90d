/* The connection to a bulletin board, as a process of its own: the name is
 * resolved and the board connected to within a time limit, then octets go
 * both ways between the board and fd until one side ends. There is no idle
 * limit: a caller reading a screen for an hour is still a caller (busybox's
 * nc -w ended such a call after two quiet quarter minutes). Called in a
 * child of the daemon (srv/bbslink.c); blocking. POSIX, C99. */
#ifndef V90D_BBSDIAL_H
#define V90D_BBSDIAL_H
#include <stdint.h>

/* host: a name or address; port: decimal; fd: a stream socket to the
 * daemon; connect_s: seconds for the name and the connect together;
 * self_ok: 0 = an address that bbsdial_is_self() names, whatever name led
 * to it, is not connected to, 1 = the tests' listener, BBSDIAL_PUBLIC =
 * only an address on the internet at large (bbsdial_is_public), for an
 * entry of Netstalgia's list of services. Whatever the name, it is looked
 * up once (IPv4 and IPv6 both) and each address it gave that passes is
 * tried in turn, each with its share of connect_s (at least 3 s).
 * Returns 0 when the board or the daemon ended the connection, 1 when the
 * board was not reached. */
#define BBSDIAL_PUBLIC 2
int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s, int self_ok);

/* Is this IPv4 address (host byte order) no board? 1 for the box itself:
 * 127/8, 0/8 and the address of any of its own interfaces (the web
 * interface listens there; a list entry or a name that leads to it would
 * put the caller's keystrokes on it); and for what is no single host
 * elsewhere: link-local 169.254/16, multicast and broadcast. The rest of
 * the local network is left to self_ok: the owner's numbers of services may
 * lead there, the lists' entries (BBSDIAL_PUBLIC) may not.
 * Sends nothing. */
int bbsdial_is_self(uint32_t addr);

/* 1 if a (IPv4, host byte order) is an address on the internet at large:
 * not 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24,
 * 192.0.2/24, 192.168/16, 198.18/15, 198.51.100/24, 203.0.113/24, nor
 * multicast, 240/4 or broadcast. */
int bbsdial_is_public(uint32_t a);

/* The same for an IPv6 address (16 octets, network order). is_self6: ::
 * and ::1, link-local fe80::/10, multicast ff00::/8, the old IPv4-
 * compatible ::/96, an IPv4-mapped ::ffff:0:0/96 whose IPv4 address is,
 * and the address of any of the box's interfaces. is_public6: global
 * unicast 2000::/3 but not the documentation 2001:db8::/32 and 3fff::/20,
 * Teredo 2001::/32, benchmarking 2001:2::/48, ORCHID 2001:10::/28 and
 * 2001:20::/28; 6to4 2002::/16 and NAT64's 64:ff9b::/96 by the IPv4
 * address in them (bbsdial_is_public); nothing else (unique local
 * fc00::/7, NAT64's local 64:ff9b:1::/48, site-local, ...). */
int bbsdial_is_self6(const uint8_t a[16]);
int bbsdial_is_public6(const uint8_t a[16]);

#endif
