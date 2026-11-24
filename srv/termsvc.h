/* The terminal menu on the daemon's lines (term/tmenu.h): a call on a PPP
 * service's own number with [services] menu on (srv.h, SRV_LINK_MENU) that
 * did not turn out to be Dial-Up Networking. One menu per port; one BBS
 * directory for both (term/tdir.h), made from the list of boards and
 * Netstalgia's board details as bbslink.c keeps them, again whenever one
 * of them changes and nobody is in the menu. Daemon only. Ours; written
 * for this project. */
#ifndef V90D_TERMSVC_H
#define V90D_TERMSVC_H
#include <stddef.h>
#include <stdint.h>

/* any port has the menu ([services] menu, on a PPP service) */
void termsvc_want(int on);
/* octets from the caller of port ctx (an srv_t *) while it is in the menu */
void termsvc_from_modem(void *ctx, const uint8_t *oct, size_t n);
/* every block: the menus, the directory's loading */
void termsvc_poll(uint32_t now_ms);
/* Who's online on the status page and the menu: what port k's call is doing */
const char *termsvc_where(int k);

#endif
