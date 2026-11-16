/* HDLC-like asynchronous framing for PPP (RFC 1662): flag 0x7e, control
 * escape 0x7d, async control character maps, FCS-16.
 *
 * Transmit: ppp_hdlc_encode() turns one frame into wire octets.
 * Receive:  ppp_hdlc_rx_byte() is a streaming decoder fed one octet at a
 *           time; it reports each complete frame whose FCS is good.
 * No allocation, no OS dependencies. */
#ifndef V90D_PPP_HDLC_H
#define V90D_PPP_HDLC_H
#include <stddef.h>
#include <stdint.h>

#define PPP_FLAG      0x7e
#define PPP_ESCAPE    0x7d
#define PPP_FCS_INIT  0xffffu   /* initial FCS value */
#define PPP_FCS_GOOD  0xf0b8u   /* FCS after running over data plus FCS */

#define PPP_MAX_MRU   1500
/* largest decoded frame: address, control, 2 protocol, information, 2 FCS */
#define PPP_HDLC_BUF  (PPP_MAX_MRU + 6)

/* Run the FCS-16 over n octets, continuing from fcs (start: PPP_FCS_INIT).
 * The transmitted FCS is the ones complement, least significant octet first. */
uint16_t ppp_fcs16(uint16_t fcs, const uint8_t *p, size_t n);

/* Encode one frame. The frame content is the concatenation of a[0..alen)
 * and b[0..blen) (either may be empty); the FCS is computed and appended.
 * Octets below 0x20 whose bit is set in accm are escaped, as are 0x7e and
 * 0x7d always. open_flag nonzero emits a leading flag (needed after an idle
 * line; a preceding frame's closing flag can serve otherwise). A closing
 * flag is always emitted. Returns the number of octets written, or 0 if the
 * result does not fit in max (nothing useful is written then). */
size_t ppp_hdlc_encode(uint8_t *out, size_t max,
                       const uint8_t *a, size_t alen,
                       const uint8_t *b, size_t blen,
                       uint32_t accm, int open_flag);

/* Exact size ppp_hdlc_encode() would produce for the same arguments. */
size_t ppp_hdlc_encoded_size(const uint8_t *a, size_t alen,
                             const uint8_t *b, size_t blen,
                             uint32_t accm, int open_flag);

/* Incremental form of the encoder, for frames assembled from several
 * pieces: begin, add any number of pieces, end. ppp_hdlc_tx_end() appends
 * the FCS and closing flag and returns the total size, or 0 if the frame
 * did not fit in max. */
typedef struct {
    uint8_t *out;
    size_t   max, n;
    uint32_t accm;
    uint16_t fcs;
} ppp_hdlc_tx_t;

void ppp_hdlc_tx_begin(ppp_hdlc_tx_t *tx, uint8_t *out, size_t max,
                       uint32_t accm, int open_flag);
void ppp_hdlc_tx_add(ppp_hdlc_tx_t *tx, const uint8_t *p, size_t n);
size_t ppp_hdlc_tx_end(ppp_hdlc_tx_t *tx);

typedef struct {
    /* configuration; may be changed between frames */
    uint32_t rx_accm;     /* raw octets < 0x20 flagged here are discarded */
    uint16_t max_frame;   /* max octets between flags after unstuffing, FCS
                             included; longer frames are dropped */
    /* result of the last good frame: buf[0..len), FCS removed. Valid from
     * the call that returned 1 until the next call. */
    uint16_t len;
    /* counters */
    uint32_t frames_ok;
    uint32_t bad_fcs;     /* complete frames failing the FCS */
    uint32_t runts;       /* 1..3 octets between flags */
    uint32_t overlong;    /* exceeded max_frame */
    uint32_t aborts;      /* ended by escape + flag */
    /* decoder state */
    uint16_t pos;
    uint16_t fcs;
    uint8_t  hunting;     /* no flag seen yet */
    uint8_t  escaped;     /* previous octet was a control escape */
    uint8_t  dropping;    /* current frame is overlong */
    uint8_t  buf[PPP_HDLC_BUF];
} ppp_hdlc_rx_t;

/* max_frame is clamped to PPP_HDLC_BUF. The receiving ACCM starts at the
 * RFC 1662 default 0xffffffff. */
void ppp_hdlc_rx_init(ppp_hdlc_rx_t *rx, size_t max_frame);

/* Feed one received octet. Returns 1 when it completed a good frame (see
 * buf/len above), else 0. */
int ppp_hdlc_rx_byte(ppp_hdlc_rx_t *rx, uint8_t octet);

/* Parse the address, control and protocol fields of a decoded frame.
 * acfc_ok / pfc_ok say whether Address-and-Control-Field-Compression and
 * Protocol-Field-Compression were negotiated for the receive direction; a
 * compressed form that was not negotiated is an error. On success returns 0
 * and sets *proto and *off (offset of the information field); -1 on a
 * malformed or disallowed header. */
int ppp_hdlc_parse(const uint8_t *frame, size_t len, int acfc_ok, int pfc_ok,
                   uint16_t *proto, size_t *off);

#endif
