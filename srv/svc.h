/* Numbers that connect a caller straight to a service on the internet: a
 * game server with a protocol of its own (raw), or a board or text service
 * (telnet). The owner's are in the configuration ([services], config.h);
 * Netstalgia keeps a list of more, signed like the list of boards
 * (bbssig.h), which the box fetches once a day (fw/bbsfetch.sh):
 *
 *     # serial 2026112400                 inside the signed part
 *     v90d-svc 1                          the first line that is not a comment
 *     svc 13855961775 nwnrevival.com:5000 raw AOL Neverwinter Nights
 *         number, host:port, raw | telnet, the name (the rest of the line)
 *     # sig ed25519 <128 hex digits>
 *
 * The fields are those of a service line of the configuration
 * (cfg_svc_parse). A line whose first word is not svc is a kind this box
 * does not know: skipped, so that Netstalgia can add kinds. A list entry is
 * further held to what Netstalgia promises: its number is none of the
 * box's own, its host no address of a private network, of the box itself
 * or of nobody, and no name that means the box (localhost); and when it is
 * called, the name must lead to a public IPv4 address (bbsdial.h). The
 * owner's entries may be on the LAN.
 *
 * Portable C99, no allocation, no globals. */
#ifndef V90D_SVC_H
#define V90D_SVC_H
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#define SVC_HEAD      "v90d-svc 1"
#define SVC_LINE_MAX  200       /* a line of the list, without its end */
#define SVC_LIST_MAX  512       /* entries the box keeps */

enum { SVC_LINE_NONE = 0,       /* empty or a comment */
       SVC_LINE_HEAD,           /* "v90d-svc 1" */
       SVC_LINE_SVC,            /* *e filled */
       SVC_LINE_OTHER,          /* a kind of line this box does not know */
       SVC_LINE_BAD };          /* an svc line that is not right, or one Netstalgia would never list */
/* One line of the list (no line end; a trailing CR is allowed). */
int svc_line(const char *line, size_t len, cfg_service_t *e);

/* 1 if the host of a list entry is acceptable as written: a name that is
 * not localhost (or under it), or a dotted address that bbsdial_is_public
 * takes. All digits and dots but not four decimal numbers is refused. */
int svc_host_ok(const char *host);

#endif
