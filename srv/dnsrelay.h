/* A DNS relay for the callers ([network] dns_relay): the box's PPP address
 * is the caller's name server, and the box asks the real ones. The callers
 * have IPv4 only, so it answers a question for IPv6 addresses (AAAA)
 * itself, with none: a caller with an IPv6 stack (Windows XP with IPv6
 * installed) then does not try addresses its link cannot carry. Every
 * other question and its answer pass as they are. Non-blocking, a slice
 * per block. Daemon only (dnsrelay.c); the message handling is pure, for
 * the tests. Ours; written for this project. */
#ifndef V90D_DNSRELAY_H
#define V90D_DNSRELAY_H
#include <stddef.h>
#include <stdint.h>

#define DNSRELAY_UP_MAX   2         /* the settings' name servers (IPv4) */
#define DNSRELAY_SRV_MAX  5         /* with the system's: asked in turn */
#define DNSRELAY_PENDING  64        /* questions in flight */
#define DNSRELAY_WAIT_MS  2000      /* before the next server is asked */
#define DNSRELAY_TRIES    3

/* the relay's settings: on, the addresses it answers on (each port's PPP
 * address; 0 = none), the callers it answers (each port's peer address:
 * a question from any other address is dropped, so the box is no name
 * server for the LAN), and the servers it asks (0 = none: those of
 * /etc/resolv.conf then) */
#define DNSRELAY_PEERS_MAX 4
void dnsrelay_set(int on, const uint32_t listen[], int n_listen, const uint32_t peers[], int n_peers,
                  const uint32_t up[DNSRELAY_UP_MAX]);
/* every block */
void dnsrelay_poll(uint32_t now_ms);

/* --- the message handling (pure) --- */
#define DNS_T_A    1
#define DNS_T_AAAA 28
/* a standard query with one question: its type, or -1 */
int    dnsrelay_qtype(const uint8_t *m, size_t n);
/* the answer "no records of that type" (NOERROR, no answers) to query q:
 * its header and question back; the length, 0 if q is not a query */
size_t dnsrelay_none(const uint8_t *q, size_t n, uint8_t *out, size_t cap);

#endif
