/* Microsoft Point-to-Point Compression (RFC 2118): the "software
 * compression" of Windows dial-up networking. Compression only; the
 * encryption that shares its option (MPPE, RFC 3078) is not implemented.
 *
 * Written from RFC 2118. A compressor for the direction we transmit and a
 * decompressor for the direction we receive, one packet per call, 8192
 * octets of history each, no allocation. The two-octet header with the
 * A/B/C/D bits and the coherency count (RFC 2118 3.1), the uncompressed
 * form for packets that expand and the CCP negotiation live in ppp.c.
 *
 * Portable C99, integer only, no OS calls. */
#ifndef V90D_PPP_MPPC_H
#define V90D_PPP_MPPC_H
#include <stddef.h>
#include <stdint.h>

#define PPP_MPPC_HIST      8192u
/* Longest packet the compressor takes: PPP_MAX_MRU octets of information
 * and two of protocol. */
#define PPP_MPPC_MAX_PKT   1502

/* RFC 2118 3.1: the bits in front of the 12-bit coherency count */
#define PPP_MPPC_FLUSHED    0x8000u   /* A: history initialised before this packet */
#define PPP_MPPC_AT_FRONT   0x4000u   /* B: this packet starts at the front of the history */
#define PPP_MPPC_COMPRESSED 0x2000u   /* C */
#define PPP_MPPC_ENCRYPTED  0x1000u   /* D: must be 0 (it is MPPE's bit, RFC 3078) */
#define PPP_MPPC_COUNT_MASK 0x0fffu

#ifndef PPP_MPPC_HASH_BITS
#define PPP_MPPC_HASH_BITS 12
#endif
/* candidates looked at for each position */
#ifndef PPP_MPPC_CHAIN
#define PPP_MPPC_CHAIN     16
#endif

typedef struct {
    uint16_t hp;                              /* where the next packet goes */
    uint8_t  flushed;                         /* history initialised, nothing sent since */
    uint16_t head[1u << PPP_MPPC_HASH_BITS];  /* hash -> position */
    uint16_t prev[PPP_MPPC_HIST];             /* position -> older one, same hash */
    uint8_t  hist[PPP_MPPC_HIST];
} ppp_mppc_enc_t;

void ppp_mppc_enc_init(ppp_mppc_enc_t *e);
/* Initialise the history (Reset-Request, RFC 2118 4.3): the next packet
 * carries the FLUSHED bit. */
void ppp_mppc_enc_flush(ppp_mppc_enc_t *e);

/* Compress one packet, a[0..an) followed by b[0..bn), 1 to
 * PPP_MPPC_MAX_PKT octets in all. Returns the number of octets written to
 * out and sets *bits to the A, B and C bits for the header. Returns 0 if
 * the result would be longer than max (RFC 2118 section 3, "Data
 * Expansion"): the packet has to go out uncompressed, *bits has the A bit
 * if it is due and no others, and the history has been initialised again
 * so that the packet after this one carries the A bit. */
size_t ppp_mppc_compress(ppp_mppc_enc_t *e,
                         const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn,
                         uint8_t *out, size_t max, unsigned *bits);

typedef struct {
    uint16_t hp;
    uint8_t  hist[PPP_MPPC_HIST];
} ppp_mppc_dec_t;

void ppp_mppc_dec_init(ppp_mppc_dec_t *d);
/* the A bit of a received packet: before it is decompressed */
#define ppp_mppc_dec_flush ppp_mppc_dec_init

/* Decompress one packet (the data after the two-octet header). at_front:
 * its B bit. Returns the length written to out, or -1 if the data is not
 * valid or does not fit in max or in the history; the caller then has to
 * resynchronise (Reset-Request). */
int ppp_mppc_decompress(ppp_mppc_dec_t *d, int at_front,
                        const uint8_t *in, size_t n, uint8_t *out, size_t max);

#endif
