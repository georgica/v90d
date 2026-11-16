/* See stun.h. */
#include <string.h>
#include "stun.h"

void stun_request(uint8_t out[STUN_REQ_LEN], const uint8_t txid[STUN_TXID_LEN])
{
    out[0] = 0x00; out[1] = 0x01;           /* Binding request */
    out[2] = 0x00; out[3] = 0x00;           /* message length: no attributes */
    out[4] = 0x21; out[5] = 0x12; out[6] = 0xA4; out[7] = 0x42;      /* magic cookie */
    memcpy(out + 8, txid, STUN_TXID_LEN);
}

int stun_parse(const uint8_t *resp, size_t len, const uint8_t txid[STUN_TXID_LEN], uint32_t *addr, uint16_t *port)
{
    size_t at;
    if (len < 20 || resp[0] != 0x01 || resp[1] != 0x01)             /* Binding success */
        return 0;
    if (resp[4] != 0x21 || resp[5] != 0x12 || resp[6] != 0xA4 || resp[7] != 0x42)
        return 0;
    if (memcmp(resp + 8, txid, STUN_TXID_LEN) != 0)
        return 0;
    at = 20;
    while (at + 4 <= len) {
        unsigned type = (unsigned)resp[at] << 8 | resp[at + 1];
        size_t alen = (size_t)resp[at + 2] << 8 | resp[at + 3];
        const uint8_t *v = resp + at + 4;
        if (at + 4 + alen > len) break;
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t p = (uint16_t)((unsigned)v[2] << 8 | v[3]);
            uint32_t a = (uint32_t)v[4] << 24 | (uint32_t)v[5] << 16 | (uint32_t)v[6] << 8 | v[7];
            if (type == 0x0020) { p ^= (uint16_t)(STUN_MAGIC >> 16); a ^= STUN_MAGIC; }
            *addr = a; *port = p;
            return 1;
        }
        at += 4 + ((alen + 3u) & ~3u);      /* attributes are padded to 4 octets */
    }
    return 0;
}
