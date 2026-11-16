/* MD5 per RFC 1321 section 3. One loop of 64 steps; the message word index
 * and rotation for each step follow the four operation tables of step 4. */
#include "ppp_md5.h"

/* T[i] = integer part of 2^32 * abs(sin(i)), i = 1..64 (RFC 1321 3.4).
 * Precomputed, since the library uses no floating point. */
static const uint32_t md5_t[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

/* left-rotation amounts, four per round, cycled */
static const uint8_t md5_rot[4][4] = {
    { 7, 12, 17, 22 }, { 5, 9, 14, 20 }, { 4, 11, 16, 23 }, { 6, 10, 15, 21 }
};

static void md5_block(uint32_t reg[4], const uint8_t *p)
{
    uint32_t x[16];
    uint32_t a = reg[0], b = reg[1], c = reg[2], d = reg[3];
    int i;

    /* message words are little-endian */
    for (i = 0; i < 16; i++)
        x[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
               ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);

    for (i = 0; i < 64; i++) {
        int round = i >> 4;
        uint32_t f, sum, tmp;
        int k, s = md5_rot[round][i & 3];

        switch (round) {
        case 0:  f = (b & c) | (~b & d); k = i;                 break;
        case 1:  f = (b & d) | (c & ~d); k = (5 * i + 1) & 15;  break;
        case 2:  f = b ^ c ^ d;          k = (3 * i + 5) & 15;  break;
        default: f = c ^ (b | ~d);       k = (7 * i) & 15;      break;
        }
        sum = a + f + x[k] + md5_t[i];
        sum = (sum << s) | (sum >> (32 - s));
        /* the roles of the registers rotate: (a,b,c,d) <- (d,b+sum,b,c) */
        tmp = d;
        d = c;
        c = b;
        b = b + sum;
        a = tmp;
    }
    reg[0] += a;
    reg[1] += b;
    reg[2] += c;
    reg[3] += d;
}

void ppp_md5_init(ppp_md5_t *m)
{
    m->reg[0] = 0x67452301u;
    m->reg[1] = 0xefcdab89u;
    m->reg[2] = 0x98badcfeu;
    m->reg[3] = 0x10325476u;
    m->len_lo = 0;
    m->len_hi = 0;
}

void ppp_md5_update(ppp_md5_t *m, const uint8_t *data, size_t len)
{
    size_t fill = m->len_lo & 63u;

    while (len > 0) {
        m->block[fill++] = *data++;
        len--;
        if (++m->len_lo == 0)
            m->len_hi++;
        if (fill == 64) {
            md5_block(m->reg, m->block);
            fill = 0;
        }
    }
}

void ppp_md5_final(ppp_md5_t *m, uint8_t digest[16])
{
    /* length in bits, captured before padding changes the byte count */
    uint32_t bits_lo = m->len_lo << 3;
    uint32_t bits_hi = (m->len_hi << 3) | (m->len_lo >> 29);
    uint8_t tail[8];
    uint8_t pad = 0x80;
    int i;

    ppp_md5_update(m, &pad, 1);
    pad = 0;
    while ((m->len_lo & 63u) != 56)
        ppp_md5_update(m, &pad, 1);
    for (i = 0; i < 4; i++) {
        tail[i] = (uint8_t)(bits_lo >> (8 * i));
        tail[4 + i] = (uint8_t)(bits_hi >> (8 * i));
    }
    ppp_md5_update(m, tail, 8);
    for (i = 0; i < 16; i++)
        digest[i] = (uint8_t)(m->reg[i >> 2] >> (8 * (i & 3)));
}
