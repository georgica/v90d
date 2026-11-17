/* V.32bis internals shared between the sources. */
#ifndef V90D_V32_PRIV_H
#define V90D_V32_PRIV_H
#include "v32.h"

extern const int8_t  v32_c4800[4][2], v32_c7200[16][2], v32_c9600[32][2],
                     v32_c12000[64][2], v32_c14400[128][2];
extern const int16_t v32_txh[10][V32_TXL][2], v32_ech[10][V32_TXL][2];
extern const int32_t v32_tx_rms_q12;
extern const int16_t v32_rxg[V32_RXP][V32_RXN][2];

const int8_t (*v32_constel(int rate))[2];
uint32_t v32_isqrt64(uint64_t v);

/* Y1Y2 label of a synchronising state (Figure 2-5) and back */
extern const uint8_t v32_abcd_label[4], v32_label_abcd[4];

#endif
