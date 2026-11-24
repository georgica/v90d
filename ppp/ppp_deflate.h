/* PPP Deflate (RFC 1979): the 'deflate' format of RFC 1951 used one PPP
 * packet at a time, with the history kept from packet to packet.
 *
 * Written from the two RFCs. A compressor for the direction we transmit
 * and a decompressor for the direction we receive; both work on whole
 * packets and never allocate. The sequence number, the choice between the
 * compressed and the native form and the CCP negotiation live in ppp.c.
 *
 * What a "packet" is here (RFC 1979 2.1): the PPP Protocol field, already
 * in its one-octet form where the number allows it, followed by the
 * Information field. The compressed form of one packet is one or more
 * deflate blocks followed by the three header bits of an empty stored
 * block and padding to the octet boundary; the four octets 00 00 FF FF
 * that would follow are not transmitted.
 *
 * Portable C99, integer only, no allocation, no OS calls. */
#ifndef V90D_PPP_DEFLATE_H
#define V90D_PPP_DEFLATE_H
#include <stddef.h>
#include <stdint.h>

/* Largest window, as the base-2 logarithm of its size (RFC 1979 section 3:
 * 8 to 15). A compile-time limit: the compressor's tables and the
 * decompressor's window are sized by it. With less than 15 we ask the peer
 * for the smaller window in our own request and use at most this much of
 * what the peer allows us. */
#ifndef PPP_DEFLATE_MAX_WBITS
#define PPP_DEFLATE_MAX_WBITS 15
#endif
#define PPP_DEFLATE_MIN_WBITS 8
#define PPP_DEFLATE_WSIZE     (1u << PPP_DEFLATE_MAX_WBITS)

/* Longest packet: PPP_MAX_MRU octets of information and two of protocol. */
#define PPP_DEFLATE_MAX_PKT   1502

/* Hash table of the compressor: 2^bits list heads of two octets each. */
#ifndef PPP_DEFLATE_HASH_BITS
#define PPP_DEFLATE_HASH_BITS 14
#endif
#define PPP_DEFLATE_HSIZE     (1u << PPP_DEFLATE_HASH_BITS)

/* Room behind the window in the compressor's buffer: the history is moved
 * down once this much has been added (at least one packet). */
#ifndef PPP_DEFLATE_WIN_EXTRA
#define PPP_DEFLATE_WIN_EXTRA 8192
#endif

/* How hard the compressor looks (ppp_deflate_enc_init). The bound on the
 * time of one call is `work`: the number of candidate strings examined
 * for one packet; when it is used up the rest of the packet is coded with
 * what the first candidate gives. */
typedef struct {
    uint16_t max_chain;   /* candidates examined per position */
    uint16_t good_len;    /* a match this long: a quarter of max_chain for
                             the lazy second look */
    uint16_t nice_len;    /* a match this long ends the search */
    uint16_t lazy_len;    /* a match this long is taken without looking at
                             the next position */
    uint32_t work;        /* candidates examined per packet, at most */
} ppp_deflate_effort_t;

typedef struct {
    ppp_deflate_effort_t effort;
    uint8_t  wbits;                   /* window in force, 8..MAX_WBITS */
    uint32_t wsize;                   /* 1 << wbits */
    uint32_t pos;                     /* octets held in win[] */
    uint16_t off;                     /* stream position of win[0], mod 65536 */
    uint32_t last_work;               /* candidates examined by the last call */
    uint16_t head[PPP_DEFLATE_HSIZE]; /* hash -> stream position, mod 65536 */
    uint16_t prev[PPP_DEFLATE_WSIZE]; /* position -> older one, same hash */
    uint8_t  win[PPP_DEFLATE_WSIZE + PPP_DEFLATE_WIN_EXTRA];
    /* the packet being coded: one entry per literal or match */
    uint16_t ntok;
    uint16_t tok_dist[PPP_DEFLATE_MAX_PKT];   /* 0 = literal */
    uint8_t  tok_val[PPP_DEFLATE_MAX_PKT];    /* literal, or length - 3 */
    uint16_t lfreq[286], dfreq[30];
    uint8_t  llen[288], dlen[30];
    uint16_t lcode[288], dcode[30];
    /* the fixed code of RFC 1951 3.2.6, bit-reversed for the writer */
    uint16_t fix_lcode[288];
    uint8_t  fix_llen[288], fix_dcode[30];
    /* counters since init */
    uint32_t blocks_dynamic, blocks_fixed, cuts;
} ppp_deflate_enc_t;

/* effort NULL: the default (ppp_deflate_effort_default). wbits is clamped
 * to 8..PPP_DEFLATE_MAX_WBITS. The history is empty. */
void ppp_deflate_effort_default(ppp_deflate_effort_t *e);
void ppp_deflate_enc_init(ppp_deflate_enc_t *e, int wbits,
                          const ppp_deflate_effort_t *effort);
/* Forget the history (Reset-Request, RFC 1979 section 2). */
void ppp_deflate_enc_reset(ppp_deflate_enc_t *e);

/* Compress one packet, a[0..an) followed by b[0..bn), 1 to
 * PPP_DEFLATE_MAX_PKT octets in all. The packet is added to the history
 * whatever the outcome (an incompressible packet goes out in its native
 * form and still counts, RFC 1979 section 2 "Data Expansion").
 * Returns the number of octets written to out (the trailing 00 00 FF FF
 * left off), or 0 if the result would be longer than max: nothing useful
 * is in out then. */
size_t ppp_deflate_compress(ppp_deflate_enc_t *e,
                            const uint8_t *a, size_t an,
                            const uint8_t *b, size_t bn,
                            uint8_t *out, size_t max);

/* One prefix code as the decompressor holds it (RFC 1951 3.2.2): the codes
 * of one length are consecutive numbers, in symbol order. */
typedef struct {
    uint16_t count[16];               /* codes of each length */
    uint16_t first[16];               /* the first code of each length */
    uint16_t index[16];               /* and its place in the symbol list */
} ppp_deflate_code_t;

typedef struct {
    uint32_t wpos;                    /* next octet of win[] to be written */
    uint32_t whave;                   /* octets of history, at most WSIZE */
    uint8_t  win[PPP_DEFLATE_WSIZE];  /* circular */
    /* the codes of the block being read, symbols in code order */
    ppp_deflate_code_t lc, dc;
    uint16_t lsym[288], dsym[32];
    int fixed;              /* lc and dc hold the fixed codes (3.2.6): not built again */
} ppp_deflate_dec_t;

void ppp_deflate_dec_init(ppp_deflate_dec_t *d);
#define ppp_deflate_dec_reset ppp_deflate_dec_init

/* Decompress one packet (as received: without the 00 00 FF FF). Returns
 * the length written to out, or -1 if the data is not a valid packet or
 * does not fit in max; the history is unchanged then and the caller has
 * to resynchronise (Reset-Request). */
int ppp_deflate_decompress(ppp_deflate_dec_t *d, const uint8_t *in, size_t n,
                           uint8_t *out, size_t max);

/* A packet that arrived in its native form: into the history. */
void ppp_deflate_dec_add(ppp_deflate_dec_t *d, const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn);

#endif
