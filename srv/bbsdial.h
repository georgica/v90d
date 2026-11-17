/* The connection to a bulletin board, as a process of its own: the name is
 * resolved and the board connected to within a time limit, then octets go
 * both ways between the board and fd until one side ends. There is no idle
 * limit: a caller reading a screen for an hour is still a caller (busybox's
 * nc -w ended such a call after two quiet quarter minutes). Called in a
 * child of the daemon (srv/bbslink.c); blocking. POSIX, C99. */
#ifndef V90D_BBSDIAL_H
#define V90D_BBSDIAL_H

/* host: a name or address; port: decimal; fd: a stream socket to the
 * daemon; connect_s: seconds for the name and the connect together;
 * self_ok: 0 = an address in 127/8 or 0/8, whatever name led to it, is not
 * connected to (the box itself is no board), 1 = the tests' listener.
 * Returns 0 when the board or the daemon ended the connection, 1 when the
 * board was not reached. */
int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s, int self_ok);

#endif
