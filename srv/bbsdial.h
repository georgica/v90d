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
 * to it, is not connected to, 1 = the tests' listener.
 * Returns 0 when the board or the daemon ended the connection, 1 when the
 * board was not reached. */
int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s, int self_ok);

/* Is this IPv4 address (host byte order) no board? 1 for the box itself:
 * 127/8, 0/8 and the address of any of its own interfaces (the web
 * interface listens there; a list entry or a name that leads to it would
 * put the caller's keystrokes on it); and for what is no single host
 * elsewhere: link-local 169.254/16, multicast and broadcast. The rest of
 * the local network is not refused: a board may well stand beside the box.
 * Sends nothing. */
int bbsdial_is_self(uint32_t addr);

#endif
