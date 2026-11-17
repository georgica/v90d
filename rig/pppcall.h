/* Rig: a whole dial-up call (modem + V.42 + PPP, dce/) behind mu-law frames,
 * with a tiny IP end point of its own so that the caller's computer has
 * something to talk to without any network set-up on the box: it answers
 * ICMP echo requests ("ping") sent to our end of the link. Test rig only. */
#ifndef V90D_RIG_PPPCALL_H
#define V90D_RIG_PPPCALL_H
#include <stdint.h>
#include "dce.h"

typedef void (*pppcall_log_fn)(const char *line);

typedef struct {
    int      level;             /* transmit RMS */
    unsigned modulations;       /* V90M_MOD_V22 | V90M_MOD_V21 | V90M_MOD_V90 */
    int      alaw;
    const char *user, *pass;    /* NULL user = no authentication */
    pppcall_log_fn log;
} pppcall_cfg;

typedef struct {
    pppcall_cfg cfg;
    dce_t       dce;
    ppp_user_t  users[1];
    dce_status_t st, shown;     /* latest, and what the log last reported */
    int         sub, alive;
    unsigned long pings, other_ip;
} pppcall_t;

#define PPPCALL_LOCAL PPP_IP4(10, 64, 64, 1)
#define PPPCALL_PEER  PPP_IP4(10, 64, 64, 2)

void pppcall_init(pppcall_t *c, const pppcall_cfg *cfg);
/* n codewords from the line in, n out */
void pppcall_frame(pppcall_t *c, const uint8_t *rx, uint8_t *tx, int n);

#endif
