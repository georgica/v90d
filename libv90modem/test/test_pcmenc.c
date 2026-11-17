/* Linear to G.711 for the telephony side of the modem, both laws, checked
 * against V.90 Table 1 (v90/v90_pcm): every codeword's own level encodes to
 * that codeword, and anything in between goes to the nearer level. */
#include "test.h"
#include "../modem/pcmenc.h"
#include "../v90/v90_pcm.h"

int main(void)
{
    int alaw, o, v;
    for (alaw = 0; alaw <= 1; alaw++) {
        int bad_self = 0, bad_near = 0;
        for (o = 0; o < 256; o++) {
            int32_t lin = v90_octet_linear(alaw, (uint8_t)o);
            uint8_t e = pcm_encode(alaw, (int16_t)lin);
            /* mu-law has two zeros; either is right */
            if (e != o && v90_octet_linear(alaw, e) != lin) bad_self++;
        }
        CHECK_EQ_I(bad_self, 0);
        for (v = -32768; v <= 32767; v += 7) {
            int32_t got = v90_octet_linear(alaw, pcm_encode(alaw, (int16_t)v));
            int32_t err = got > v ? got - v : v - got, best = 1 << 30;
            for (o = 0; o < 256; o++) {
                int32_t l = v90_octet_linear(alaw, (uint8_t)o), d = l > v ? l - v : v - l;
                if (d < best) best = d;
            }
            if (err != best) bad_near++;
        }
        CHECK_EQ_I(bad_near, 0);
        CHECK_EQ_I(v90_octet_linear(alaw, pcm_encode(alaw, 32767)), alaw ? 32256 : 32124);
        CHECK_EQ_I(v90_octet_linear(alaw, pcm_encode(alaw, -32768)), alaw ? -32256 : -32124);
    }
    T_REPORT("pcmenc");
}
