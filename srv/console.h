/* What is done to the IP packets of a game console on their way from the
 * telephone line to the network (service = dreamcast, cfg/config.h). The
 * facts behind both rules are in refs/dreampi-facts.md; neither has been
 * tried with a real console.
 *
 *   - The hop count (TTL) is set to 64. The console is said to send its
 *     packets with about 30, which does not reach every game server.
 *   - A TCP connection opened from the source ports 3200..3205 whose
 *     opening packet (SYN) is repeated to the same destination port within a
 *     second is passed on once.
 *   - An opening packet (SYN) that also carries PSH loses the PSH. Some
 *     games open connections that way and some servers drop it.
 *   - The last name the console asked a name server for (an A question to
 *     UDP port 53) is remembered, for the "Dreamcast Now" report.
 *
 * Portable C99, integers only, no allocation. */
#ifndef V90D_SRV_CONSOLE_H
#define V90D_SRV_CONSOLE_H
#include <stddef.h>
#include <stdint.h>

#define CONSOLE_SYN_LO 3200
#define CONSOLE_SYN_HI 3205
#define CONSOLE_SYN_MS 1000u
#define CONSOLE_TTL    64
#define CONSOLE_NAME_MAX 253

typedef struct {
    struct { uint8_t seen; uint16_t dport; uint32_t ms; } syn[CONSOLE_SYN_HI - CONSOLE_SYN_LO + 1];
    char     name[CONSOLE_NAME_MAX + 1];    /* the last name asked for, "" before the first */
    uint32_t name_seq;                      /* how many have been seen */
} console_t;

void console_init(console_t *k);
/* One packet from the caller, len octets, at now_ms on any millisecond clock
 * (it may wrap). Mends it in place; 1 = pass it on, 0 = drop it. What is
 * not a whole IPv4 header is passed on untouched. */
int  console_from_caller(console_t *k, uint8_t *pkt, size_t len, uint32_t now_ms);

/* The last name asked for (never null) and, in *seq, how many names have
 * been seen so far: a caller that remembers the number can tell a new one. */
const char *console_name(const console_t *k, uint32_t *seq);

/* the name servers a console is given: those of the game revival projects,
 * host byte order */
#define CONSOLE_DNS1 0x2E655B7Bu    /* 46.101.91.123 */
#define CONSOLE_DNS2 0xD1323281u    /* 209.50.50.129 */

#endif
