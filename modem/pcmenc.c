#include "pcmenc.h"
#include "../v90/v90_pcm.h"

uint8_t pcm_encode(int alaw, int16_t v)
{
    /* the levels rise with the Ucode: binary search for the nearest */
    int32_t m = v < 0 ? -(int32_t)v : v, a, b;
    unsigned lo = 0, hi = 127;
    while (lo < hi) {
        unsigned mid = (lo + hi + 1) / 2;
        if (v90_ucode_linear(alaw, mid) <= m) lo = mid; else hi = mid - 1;
    }
    a = m - v90_ucode_linear(alaw, lo);
    if (lo < 127) {
        b = v90_ucode_linear(alaw, lo + 1) - m;
        if (b < a) lo++;
    }
    return v90_pcm_octet(alaw, lo, v >= 0);
}
