/* G.711 helpers, integer only. Ucode numbering follows V.90 Table 1:
 * Ucode u (0..127) with positive sign is mu-law octet 0xFF - u, negative is 0x7F - u. */
#ifndef V90D_G711_H
#define V90D_G711_H
#include <stdint.h>

/* Linear value (16-bit scale, as in V.90 Table 1) of a Ucode magnitude. */
static inline int32_t ucode_linear(unsigned u)
{
    unsigned seg = (u >> 4) & 7u, step = u & 15u;
    return (int32_t)((((2u * step + 33u) << seg) - 33u) << 2);
}

static inline uint8_t ucode_to_ulaw(unsigned u, int positive)
{
    return (uint8_t)((positive ? 0xFFu : 0x7Fu) - (u & 0x7Fu));
}

static inline int32_t ulaw_to_linear(uint8_t b)
{
    int32_t v = ucode_linear((unsigned)(~b) & 0x7Fu);
    return (b & 0x80u) ? v : -v;
}

static inline uint8_t linear_to_ulaw(int32_t v)
{
    int pos = v >= 0;
    uint32_t m = (uint32_t)(pos ? v : -v);
    unsigned seg = 0, u;
    m >>= 2;
    if (m > 8158u) m = 8158u;
    m += 33u;
    while ((m >> (seg + 6)) != 0 && seg < 7) seg++;
    u = (seg << 4) | ((m >> (seg + 1)) & 15u);
    return ucode_to_ulaw(u, pos);
}

/* A-law, used only to simulate a mu->A->mu transcoding impairment. */
static inline uint8_t linear_to_alaw(int32_t v)
{
    int pos = v >= 0;
    uint32_t m = (uint32_t)(pos ? v : -v) >> 3; /* 13-bit magnitude */
    unsigned seg = 0, a;
    if (m > 4095u) m = 4095u;
    if (m >= 32u) {
        seg = 1;
        while ((m >> (seg + 5)) != 0 && seg < 7) seg++;
        a = (seg << 4) | ((m >> seg) & 15u);
    } else {
        a = m >> 1;
    }
    return (uint8_t)((a | (pos ? 0x80u : 0u)) ^ 0x55u);
}

static inline int32_t alaw_to_linear(uint8_t b)
{
    unsigned a = b ^ 0x55u, seg = (a >> 4) & 7u, step = a & 15u;
    int32_t m = seg ? (int32_t)(((2u * step + 33u) << (seg - 1)) << 3) : (int32_t)((2u * step + 1u) << 3);
    return (a & 0x80u) ? m : -m;
}
#endif
