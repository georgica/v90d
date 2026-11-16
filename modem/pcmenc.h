/* Linear sample to the nearest G.711 codeword (V.90 Table 1 levels), for the
 * parts of a call that are generated as linear samples (V.8, Phase 2, the
 * slow modulations). Integer only. */
#ifndef V90D_MODEM_PCMENC_H
#define V90D_MODEM_PCMENC_H
#include <stdint.h>

uint8_t pcm_encode(int alaw, int16_t v);

#endif
