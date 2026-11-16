/* MD5 message digest (RFC 1321), written from the RFC's algorithm
 * description (section 3). Used only for CHAP-MD5 and for stirring the
 * session's nonce generator. No allocation, no OS dependencies. */
#ifndef V90D_PPP_MD5_H
#define V90D_PPP_MD5_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t reg[4];    /* registers A, B, C, D */
    uint32_t len_lo;    /* message length in bytes, low and high words */
    uint32_t len_hi;
    uint8_t  block[64]; /* partial input block */
} ppp_md5_t;

void ppp_md5_init(ppp_md5_t *m);
void ppp_md5_update(ppp_md5_t *m, const uint8_t *data, size_t len);
void ppp_md5_final(ppp_md5_t *m, uint8_t digest[16]);

#endif
