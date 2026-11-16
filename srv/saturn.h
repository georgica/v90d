/* Two game consoles that would telephone each other, joined over the
 * network instead (service = saturn: the Sega Saturn's NetLink modem, and
 * whatever else plays modem to modem). Each player dials into a box of
 * their own; the two boxes carry the octets of the modems to each other in
 * UDP datagrams. The words on the network are those of the tunnel that the
 * DreamPi project uses (refs/dreampi-facts.md), so the other player may
 * just as well have one of those.
 *
 * What is dialled says which side we are:
 *     0 or 00              wait for the other player to call (direct)
 *     #a*b*c*d#            call the player at IPv4 address a.b.c.d (direct)
 *     # twelve digits #    the same, each part written with three digits
 *     #0NN#                wait to be paired for game NN (matchmaking)
 *     any other #...#      call, paired for game = the last two characters
 *
 * Direct: the caller's box opens a TCP connection to the waiting one, port
 * 65432, and says `readyip`; the answer is `g2gip`. From then on UDP: the
 * waiting side from port 20001 to the caller's 20002 and back.
 *
 * Matchmaking: each side learns its own public address and port by STUN
 * (srv/stun.h) and asks a match server (saturn.dreampipe.net) to pair it
 * with another box dialling the same game. There is no TCP; both sides go
 * straight to UDP against the public address the server returns. It is off
 * unless the owner turns it on, since it makes the box a client of that
 * server.
 *
 * This file is the words only: no sockets. Portable C99, no allocation. */
#ifndef V90D_SATURN_H
#define V90D_SATURN_H
#include <stddef.h>
#include <stdint.h>

enum { SATURN_NONE = 0, SATURN_WAIT, SATURN_CALL, SATURN_MATCH_WAIT, SATURN_MATCH_CALL };

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

/* saturn.dreampipe.net: the match server, reached over HTTPS. The query is
 * GET SATURN_MATCH_PATH?action=...&gameID=GG&client_ip=DDDDDDDDDDDD&port=P&key=...
 * with a browser-like User-Agent (a short one is refused). client_ip is the
 * public address as twelve digits, each octet three digits, no dots. */
#define SATURN_MATCH_HOST  "saturn.dreampipe.net"
#define SATURN_MATCH_PATH  "/match_service.php"
#define SATURN_MATCH_KEY   "9c472a1b"        /* the fixed key, from the public NetLink source */
#define SATURN_MATCH_AGENT "Mozilla/5.0 (compatible; v90d NetLink)"
#define SATURN_STUN_HOST   "stun.l.google.com"
#define SATURN_STUN_PORT   19302

/* What the dialled digits (as tel.h collects them: a closing # is not among
 * them) ask for: SATURN_WAIT or SATURN_CALL (direct) with the address in
 * *addr (host order); SATURN_MATCH_WAIT or SATURN_MATCH_CALL (matchmaking)
 * with the two-character game id in game[0..2] (NUL-terminated); or
 * SATURN_NONE. game may be NULL if the caller does not want it. */
int saturn_number(const char *dialed, uint32_t *addr, char game[3]);

/* An answer from the match server (JSON). When its "status" equals
 * want_status, the opponent's "ip_address" (twelve digits, a space, the
 * port) is read into *addr (host order) and *port, and 1 is returned; else
 * 0. Robust to spaces and key order; no allocation. */
int saturn_match_parse(const char *json, size_t len, const char *want_status, uint32_t *addr, uint16_t *port);

/* The twelve-digit form of an address (each octet three digits, no dots)
 * into out[0..13). */
void saturn_ip12(uint32_t addr, char out[13]);

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
