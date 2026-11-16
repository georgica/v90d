/* Two game consoles that would telephone each other, joined over the
 * network instead (service = saturn: the Sega Saturn's NetLink modem, and
 * whatever else plays modem to modem). Each player dials into a box of
 * their own; the two boxes carry the octets of the modems to each other in
 * UDP datagrams. The words on the network are those of the tunnel that the
 * DreamPi project uses (refs/dreampi-facts.md), so the other player may
 * just as well have one of those.
 *
 * What is dialled says which side we are:
 *     0 or 00              wait for the other player to call
 *     #a*b*c*d#            call the player at IPv4 address a.b.c.d
 *     # twelve digits #    the same, each part written with three digits
 * (The numbers that ask a matchmaking service for an opponent are not
 * understood.)
 *
 * The caller's box opens a TCP connection to the waiting one, port 65432,
 * and says `readyip`; the answer is `g2gip`. From then on UDP: the waiting
 * side from port 20001 to the caller's 20002 and back.
 *
 * This file is the words only: no sockets. Portable C99, no allocation. */
#ifndef V90D_SATURN_H
#define V90D_SATURN_H
#include <stddef.h>
#include <stdint.h>

enum { SATURN_NONE = 0, SATURN_WAIT, SATURN_CALL };

#define SATURN_TCP_PORT   65432
#define SATURN_UDP_WAIT   20001     /* the waiting side's port */
#define SATURN_UDP_CALL   20002     /* the calling side's */
#define SATURN_HELLO      "readyip"
#define SATURN_HELLO_ACK  "g2gip"
#define SATURN_PING       "PING_SHIRO"
#define SATURN_PONG       "PONG_SHIRO"
#define SATURN_OPEN       "OPEN_SHIRO"
#define SATURN_TOS        184
/* what the waiting player's game is shown once the other one is there: it
 * takes them for the result codes of its own modem */
#define SATURN_RING       "RING\r\nCONNECT\r\n"

#define SATURN_KEEP       5         /* records in a datagram: the newest and the four before it */
#define SATURN_REC_MAX    256       /* octets we put into one record */
#define SATURN_JUMP_MAX   4096      /* a record number further ahead than this is not believed */
#define SATURN_DGRAM_MAX  (SATURN_KEEP * (SATURN_REC_MAX + 11 + 10) + (SATURN_KEEP - 1) * 13)

/* What the dialled digits (as tel.h collects them: a closing # is not
 * among them) ask for: SATURN_WAIT, SATURN_CALL with the address in *addr
 * (host order), or SATURN_NONE. */
int saturn_number(const char *dialed, uint32_t *addr);

/* a datagram that is not data: 1 = ping (answer with SATURN_PONG), 2 =
 * anything else to be ignored (pong, open); 0 = data */
int saturn_control(const uint8_t *dg, size_t len);

typedef struct {
    uint8_t  rec[SATURN_KEEP][SATURN_REC_MAX];
    uint16_t len[SATURN_KEEP];
    uint32_t seq;               /* of the next record */
} saturn_tx;

typedef struct { uint32_t expect; } saturn_rx;

void   saturn_tx_init(saturn_tx *t);
void   saturn_rx_init(saturn_rx *r);
/* The next n octets from our modem (1..SATURN_REC_MAX) as a datagram of at
 * most SATURN_DGRAM_MAX octets in out: they and the records before them.
 * Returns its length. Every datagram is to be sent twice. */
size_t saturn_pack(saturn_tx *t, const uint8_t *oct, size_t n, uint8_t *out);
/* A data datagram from the other side: the octets for our modem that it
 * brings (none when all of it was seen before), in order, into out; at most
 * cap, what does not fit is lost. A datagram whose newest record is more
 * than SATURN_JUMP_MAX ahead is dropped whole. Returns how many. */
size_t saturn_unpack(saturn_rx *r, const uint8_t *dg, size_t len, uint8_t *out, size_t cap);

#endif
