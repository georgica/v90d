/* The little of STUN (RFC 5389) that NetLink matchmaking needs: a Binding
 * request, and the public address and port out of the Binding success the
 * server sends back. One request, one reply, UDP; the socket I/O is the
 * caller's. Portable C99, no allocation. */
#ifndef V90D_STUN_H
#define V90D_STUN_H
#include <stddef.h>
#include <stdint.h>

#define STUN_REQ_LEN   20
#define STUN_TXID_LEN  12
#define STUN_MAGIC     0x2112A442u

/* Build a Binding request (no attributes) with the given transaction id. */
void stun_request(uint8_t out[STUN_REQ_LEN], const uint8_t txid[STUN_TXID_LEN]);

/* A Binding success for our transaction id: the mapped address (host order)
 * and port into *addr and *port, 1; else 0. Reads XOR-MAPPED-ADDRESS, and
 * MAPPED-ADDRESS if that is all the server gives. IPv4 only. */
int stun_parse(const uint8_t *resp, size_t len, const uint8_t txid[STUN_TXID_LEN], uint32_t *addr, uint16_t *port);

#endif
