/* MPPE (Microsoft Point-to-Point Encryption, RFC 3078): the packet path.
 * Keyed from the MS-CHAPv2 exchange (ppp_crypto's master keys); negotiated
 * inside CCP option 18. Both modes, per direction:
 *
 * Stateless (historyless, the H bit): the key changes before every packet.
 * What pppd asks for, and Windows on its VPN links.
 *
 * Stateful: one RC4 stream runs on from packet to packet; the key changes
 * on "flag" packets, whose count ends in 0xFF (RFC 3078 7.2). It is what a
 * Windows client asks for on a modem link, and the only mode it does there
 * (issue #6). FLUSHED means "RC4 started again from the current key"
 * (RFC 3078 section 3), not a key change: Windows sets it on its first
 * packet and after a Reset-Request, and leaves it off its flag packets.
 * A receiver that loses step drops packets and sends one CCP Reset-Request
 * until a FLUSHED (or flag) packet comes (8.2); no Reset-Ack is used.
 *
 * The key schedule is the one the Linux kernel's ppp_mppe uses, which is
 * what pppd and the Windows clients interoperate with: each direction starts
 * from ppp_mppe_initial_key, its coherency count starts at 4095 (so the first
 * packet is number 0), and a key change (ppp_mppe_change_key, with its RC4
 * step) comes before every packet, the first one included.
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
    uint16_t coherency;     /* 12-bit count of the last packet sent / received */
    uint8_t  stateful;      /* this direction runs stateful (H bit clear) */
    uint8_t  discard;       /* stateful receive: lost step, dropping until a
                               FLUSHED or flag packet (RFC 3078 8.2) */
    ppp_rc4_t rc4;          /* stateful: the running RC4 stream */
    uint16_t credit;        /* stateless receive: key changes that may be spent catching up
                               over lost packets (PPP_MPPE_CREDIT_*) */
} ppp_mppe_dir_t;

typedef struct {
    ppp_mppe_dir_t tx, rx;
    int on;                 /* both directions keyed and running */
} ppp_mppe_t;

/* MPPE packet header flags (the two octets after PPP protocol 0x00FD) */
#define PPP_MPPE_FLUSHED 0x8000u   /* A: RC4 table re-initialised (always set, stateless) */
#define PPP_MPPE_ENCRYPT 0x1000u   /* D: the packet is encrypted */
#define PPP_MPPE_COUNT   0x0fffu   /* coherency count */

/* An MPPE frame is this much longer than the IP packet in it: the 2-octet
 * MPPE header and the 2-octet PPP protocol field inside the ciphertext. A
 * link that encrypts carries IP packets of at most the peer's MRU less this. */
#define PPP_MPPE_OVERHEAD 4

/* How far ahead of the last received packet a count may be and still be
 * followed (one key change per step): less than half the 4096 counts, as in
 * the Linux kernel. A count at or behind the last one, or further ahead, is
 * a replay or a late packet and is dropped. */
#define PPP_MPPE_WINDOW 2047u
/* A key change costs two SHA-1 blocks and an RC4 key set-up: a peer that
 * jumps the count ahead in every packet would make the loop do thousands
 * for each. Catching up over lost packets is paid for out of a credit that
 * every packet adds PPP_MPPE_CREDIT_STEP to, up to PPP_MPPE_CREDIT_MAX: a
 * burst of real loss after ordinary traffic is followed; a stream of jumps
 * costs a few key changes a packet, and a jump the credit does not cover is
 * dropped (keys kept). */
#define PPP_MPPE_CREDIT_STEP 8u
#define PPP_MPPE_CREDIT_MAX  512u

/* Set both directions up from the server's master start keys (send/recv),
 * key length 8 or 16, with the short-key forcing (PPP_MPPE_FORCE_*). */
void ppp_mppe_begin(ppp_mppe_t *m, const uint8_t send_start[16], const uint8_t recv_start[16],
                    size_t keylen, int force);
/* Make either direction stateful (RFC 3078 7.2); right after ppp_mppe_begin,
 * before any packet. The first stateful packet (count 0) is encrypted with
 * the initial key itself, as the Linux kernel does. */
void ppp_mppe_stateful(ppp_mppe_t *m, int tx, int rx);

/* The peer sent a CCP Reset-Request. Stateless: nothing to do (every packet
 * is FLUSHED). Stateful: the next packet goes out FLUSHED so the peer can
 * resynchronise. RFC 3078 8.2 restarts RC4 on the CURRENT key for that,
 * which would send the start of a keystream already used; instead the
 * count skips ahead to the next flag packet, which changes the key (the
 * skipped counts look like lost packets to the peer, who is already
 * waiting for a FLUSHED one). */
void ppp_mppe_tx_resync(ppp_mppe_t *m);

/* Stateful receive: nonzero while out of step (a packet was lost; the peer
 * should be sent a CCP Reset-Request). ppp_mppe_rx_lost puts the receiver
 * out of step by hand, for a packet that decrypted to nonsense. */
int ppp_mppe_rx_out_of_step(const ppp_mppe_t *m);
void ppp_mppe_rx_lost(ppp_mppe_t *m);

/* Forget both directions' keys (wiped). */
void ppp_mppe_reset(ppp_mppe_t *m);

/* Encrypt one datagram (the octets that follow 0x00FD: the encapsulated PPP
 * protocol and its payload). Writes the 2-octet MPPE header and the
 * ciphertext to out; returns the total length, or 0 if it does not fit. */
size_t ppp_mppe_encrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max);

/* Decrypt one received MPPE datagram (2-octet header then ciphertext).
 * Writes the plaintext to out; returns its length, or -1 on a bad header, a
 * frame that does not fit, or a count outside the window (nothing changes
 * then: the next good packet is still followed). Stateful, -1 also when the
 * count shows a lost packet, and for every packet after it until one that
 * can be decrypted again (FLUSHED or a flag packet): see
 * ppp_mppe_rx_out_of_step. */
int ppp_mppe_decrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max);

#endif
