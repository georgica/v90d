#include "g711.h"

/* V.90 Table 1 grid: magnitude u = (seg<<4)|step, seg 0..7, step 0..15.
 * linear = ((((2*step + 33) << seg) - 33) << 2). */
int32_t ucode_linear(unsigned u)
{
    unsigned seg = (u >> 4) & 7u, step = u & 15u;
    return (int32_t)((((2u * step + 33u) << seg) - 33u) << 2);
}

uint8_t ucode_to_ulaw(unsigned u, int positive)
{
    return (uint8_t)((positive ? 0xFFu : 0x7Fu) - (u & 0x7Fu));
}

int32_t ulaw_to_linear(uint8_t b)
{
    int32_t v = ucode_linear((unsigned)(~b) & 0x7Fu);
    return (b & 0x80u) ? v : -v;
}

uint8_t linear_to_ulaw(int32_t v)
{
    int pos = v >= 0;
    uint32_t m = (uint32_t)(pos ? v : -v);
    unsigned seg = 0, u;
    m >>= 2;                 /* to the Table 1 pre-scale */
    if (m > 8158u) m = 8158u;
    m += 33u;                /* add bias */
    while ((m >> (seg + 6)) != 0 && seg < 7) seg++;
    u = (seg << 4) | ((m >> (seg + 1)) & 15u);
    return ucode_to_ulaw(u, pos);
}
