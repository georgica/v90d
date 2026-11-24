/* The time service on the box's lines (srv/acts.h): a call to SRV_ACTS
 * (srv.h, SRV_LINK_ACTS) gets ACTS-format time from the box's clock.
 * Daemon only. Ours; written for this project. */
#ifndef V90D_ACTSVC_H
#define V90D_ACTSVC_H
#include <stddef.h>
#include <stdint.h>

/* octets from the caller of port ctx (an srv_t *) on a time service call */
void actsvc_from_modem(void *ctx, const uint8_t *oct, size_t n);
/* every block: the lines, the OTMs, the end of the call */
void actsvc_poll(void);

#endif
