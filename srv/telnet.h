/* The telnet words (RFC 854, 855) for the bbs service: the box is the
 * client, between a caller's terminal program, which knows nothing of
 * telnet, and a bulletin board.
 *
 * Nothing is asked of the board unasked. What it offers or asks for is
 * answered: it may echo (RFC 857) and suppress go-ahead (858), either side
 * may go binary (856), we tell a terminal type "ANSI" (1091) and a window
 * of 80 x 24 (1073); everything else is refused. An answer is given only
 * when it changes something, so no two sides can talk for ever.
 *
 * Portable C99, no allocation, no globals. */
#ifndef V90D_TELNET_H
#define V90D_TELNET_H
#include <stddef.h>
#include <stdint.h>

#define TELNET_REPLY_MAX 16     /* the longest answer to one command */

typedef struct {
    int     st;                 /* where in a command we are */
    uint8_t cmd;
    uint8_t sb_opt, sb_first; int sb_n;
    int     him_bin, him_echo, him_sga;     /* what the board does */
    int     us_bin, us_sga, us_ttype, us_naws;   /* what we do */
    int     cr_rx, cr_tx;       /* the last octet was a CR (and the side is not binary) */
} telnet_t;

void   telnet_init(telnet_t *t);
/* Octets from the board. Returns how many of in[0..n) were taken; all of
 * them unless out or rep ran full. out[0..*out_n) is for the caller (never
 * more than taken), rep[0..*rep_n) goes back to the board. Give rep at
 * least TELNET_REPLY_MAX of room, or nothing is taken. */
size_t telnet_rx(telnet_t *t, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_n,
                 uint8_t *rep, size_t rep_cap, size_t *rep_n);
/* Octets from the caller, as the board must be sent them: 255 doubled, and
 * NUL after CR while we are not binary. Returns how many of in were taken;
 * out[0..*out_n), out_cap at least 2. */
size_t telnet_tx(telnet_t *t, const uint8_t *in, size_t n, uint8_t *out, size_t out_cap, size_t *out_n);

#endif
