/* Van Jacobson TCP/IP header compression (RFC 1144), for PPP (RFC 1332:
 * IPCP option 2, IP-Compression-Protocol 0x002d).
 *
 * The compressor keeps the last TCP/IP header of up to PPP_VJ_SLOTS
 * connections and sends, for an ordinary packet of one of them, only what
 * changed: PPP protocol 0x002d (compressed TCP). A packet that starts or
 * re-states a connection goes whole with the slot number in place of the
 * IP protocol field: 0x002f (uncompressed TCP). Anything else (not TCP, a
 * fragment, SYN, FIN, RST, no ACK, an IP header with options that changed)
 * goes as plain IP, 0x0021. The decompressor rebuilds the headers from the
 * same slots. After a frame that may have been lost (bad FCS, overrun) the
 * receiver tosses compressed packets until one names its slot, or an
 * uncompressed one comes (RFC 1144 section 4.2).
 *
 * Each direction stands alone: the peer's IPCP request says how many slots
 * it keeps and whether the slot number may be left out (our transmit
 * side); ours says the same for our receive side.
 *
 * Written from RFC 1144; integer only, no allocation. */
#ifndef V90D_PPP_VJ_H
#define V90D_PPP_VJ_H
#include <stddef.h>
#include <stdint.h>

#define PPP_VJ_SLOTS     16      /* at most, each way (RFC 1144: 16) */
#define PPP_VJ_HDR_MAX   128     /* a TCP/IP header kept: IP and TCP, options included */

#define PPP_PROTO_VJC    0x002d  /* compressed TCP */
#define PPP_PROTO_VJUC   0x002f  /* uncompressed TCP (the slot in the IP protocol field) */

typedef struct {
    uint8_t  hdr[PPP_VJ_HDR_MAX];
    uint16_t hlen;              /* 0: the slot holds nothing yet */
} ppp_vj_slot_t;

typedef struct {
    /* transmit (compressor) */
    ppp_vj_slot_t tx[PPP_VJ_SLOTS];
    uint8_t  tx_slots;          /* slots the peer keeps, 1..16; 0 = off */
    uint8_t  tx_cid_comp;       /* the peer lets the slot number be left out */
    uint8_t  tx_lru[PPP_VJ_SLOTS];  /* slot numbers, most recently used first */
    int      tx_last;           /* slot of the last packet sent, -1 none */
    /* receive (decompressor) */
    ppp_vj_slot_t rx[PPP_VJ_SLOTS];
    uint8_t  rx_slots;          /* slots we keep, 1..16; 0 = off */
    int      rx_last;           /* slot of the last packet received, -1 none */
    uint8_t  rx_toss;           /* a frame was lost: compressed packets tossed
                                   until one names its slot */
} ppp_vj_t;

/* Transmit side, from the peer's IPCP option: its slot count (Max-Slot-Id
 * + 1; more than PPP_VJ_SLOTS uses PPP_VJ_SLOTS) and its Comp-Slot-Id.
 * slots 0 turns it off. Every slot starts empty. */
void ppp_vj_tx_init(ppp_vj_t *v, unsigned slots, int cid_comp);
/* Receive side, from our own IPCP option (1..16 slots; 0 off). */
void ppp_vj_rx_init(ppp_vj_t *v, unsigned slots);

/* Compress one IP datagram for sending. The packet to send is written to
 * out (at most max octets; max >= len is always enough) and its PPP
 * protocol (PPP_PROTO_VJC, PPP_PROTO_VJUC or 0x0021) returned; *outlen is
 * its length. With the transmit side off, or a datagram that cannot be
 * compressed, it is copied as it is, as 0x0021. 0 when max is too small. */
uint16_t ppp_vj_compress(ppp_vj_t *v, const uint8_t *ip, size_t len,
                         uint8_t *out, size_t max, size_t *outlen);

/* Rebuild the IP datagram of a received PPP_PROTO_VJC or PPP_PROTO_VJUC
 * packet into out (at most max octets). Its length, or -1 when the packet
 * is malformed, names a slot that holds nothing, or is tossed (after
 * ppp_vj_rx_lost) -- drop it and count it. */
int ppp_vj_uncompress(ppp_vj_t *v, uint16_t proto, const uint8_t *in, size_t n,
                      uint8_t *out, size_t max);

/* A frame was lost or damaged on the way in (RFC 1144 4.2): compressed
 * packets are tossed until one names its slot. */
void ppp_vj_rx_lost(ppp_vj_t *v);

#endif
