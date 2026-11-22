/* MPPE (Microsoft Point-to-Point Encryption, RFC 3078): the packet path.
 * Keyed from the MS-CHAPv2 exchange (ppp_crypto's master keys); negotiated
 * inside CCP option 18 alongside MPPC. Stateless (historyless) mode only:
 * the key changes on every packet, which suits a link with no error control
 * (the same reason CCP runs MPPC historyless here).
 *
 * Written from RFC 3078 / RFC 3079; integer only, no allocation. */
#ifndef V90D_PPP_MPPE_H
#define V90D_PPP_MPPE_H
#include <stddef.h>
#include <stdint.h>
#include "ppp_crypto.h"

/* one direction */
typedef struct {
    uint8_t  start[16];     /* the master start key for this direction */
    uint8_t  session[16];   /* the current session key */
    size_t   keylen;        /* 8 (40- or 56-bit) or 16 (128-bit) */
    int      force;         /* PPP_MPPE_FORCE_* for the short keys */
    uint16_t coherency;     /* 12-bit packet counter */
} ppp_mppe_dir_t;

typedef struct {
    ppp_mppe_dir_t tx, rx;
    int on;                 /* both directions keyed and running */
} ppp_mppe_t;

/* MPPE packet header flags (the two octets after PPP protocol 0x00FD) */
#define PPP_MPPE_FLUSHED 0x8000u   /* A: table re-initialised (always set, stateless) */
#define PPP_MPPE_ENCRYPT 0x1000u   /* D: the packet is encrypted */
#define PPP_MPPE_COUNT   0x0fffu   /* coherency count */

/* Set both directions up from the server's master start keys (send/recv),
 * key length 8 or 16, with the short-key forcing (PPP_MPPE_FORCE_*). */
void ppp_mppe_begin(ppp_mppe_t *m, const uint8_t send_start[16], const uint8_t recv_start[16],
                    size_t keylen, int force);
void ppp_mppe_reset(ppp_mppe_t *m);

/* Encrypt one datagram (the octets that follow 0x00FD: the encapsulated PPP
 * protocol and its payload). Writes the 2-octet MPPE header and the
 * ciphertext to out; returns the total length, or 0 if it does not fit. */
size_t ppp_mppe_encrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max);

/* Decrypt one received MPPE datagram (2-octet header then ciphertext).
 * Writes the plaintext to out; returns its length, or -1 on a bad header or
 * a coherency count that cannot be followed. */
int ppp_mppe_decrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max);

#endif
