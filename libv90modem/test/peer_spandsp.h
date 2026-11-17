/* Thin wrapper keeping spandsp's headers (whose names clash with ours) in their
 * own translation unit. Test peer only. */
#ifndef V90D_PEER_SPANDSP_H
#define V90D_PEER_SPANDSP_H
#include <stdint.h>

/* spandsp's own modulation bit numbering, for building/reading its menus */
#define SP_MOD_V21 (1 << 1)
#define SP_MOD_V22 (1 << 2)
#define SP_MOD_V32 (1 << 9)
#define SP_MOD_V34 (1 << 11)
#define SP_MOD_V90 (1 << 12)

typedef struct {
    int final, ok;            /* negotiation finished / V.8 call established */
    int call_function, modulations, protocol, pstn_access, pcm;
} sp_v8_result;

void sp_v8call_start(unsigned mods, int pcm, int send_ci);
void sp_v8call_tick(int16_t rx, int16_t *tx);
const sp_v8_result *sp_v8call_result(void);
void sp_v8call_stop(void);

/* V.22bis peer: bit_rate 2400 or 1200, calling != 0 for the calling modem.
 * get_bit supplies its transmit data; put_bit receives decoded data bits
 * (spandsp's status indications are filtered out). */
void sp_v22_start(int bit_rate, int calling, int (*get_bit)(void *), void (*put_bit)(void *, int), void *user);
void sp_v22_tick(int16_t rx, int16_t *tx);
int  sp_v22_rate(void);          /* current bit rate */
int  sp_v22_trained(void);       /* spandsp reported training succeeded */
/* ask spandsp to start a retrain (its v22bis_request_retrain); returns what it returns */
int  sp_v22_retrain(int bit_rate);
/* status indications seen so far: 0 = training succeeded, 1 = training in progress, 2 = training failed */
int  sp_v22_count(int what);
void sp_v22_stop(void);

#endif
