/* Thin wrapper keeping spandsp's headers (whose v42_* and hdlc_* names would
 * clash with ours) in their own translation unit. Test peer only, host only;
 * nothing here is linked into the library. One V.42 instance and one
 * stand-alone V.42 bis instance, both static inside the wrapper. */
#ifndef V90D_PEER_SPANDSP_V42_H
#define V90D_PEER_SPANDSP_V42_H
#include <stdint.h>

typedef int  (*sp_get_fn)(void *user, uint8_t *buf, int max);       /* returns octets supplied */
typedef void (*sp_put_fn)(void *user, const uint8_t *buf, int len);

/* ---- spandsp V.42 endpoint (it times itself in transmit bits at 28800 bit/s)
 * calling: 1 = originator. detect: 1 = run the detection phase.
 * p0: 0 leaves spandsp as it is (it asks for V.42 bis P0=1, P1=512, P2=6 in
 * its XID but passes user data through untouched). 1..3 writes that P0 (and
 * p1, p2) into its XID parameters and runs spandsp's own V.42 bis around its
 * data callbacks in the directions P0 names, which spandsp's V.42 does not
 * do by itself. -1 makes its XID carry no compression parameters at all.
 * get/put carry plain user data. */
void sp_v42_start(int calling, int detect, int p0, int p1, int p2,
                  sp_get_fn get, sp_put_fn put, void *user);
void sp_v42_rx_bit(int bit);
int  sp_v42_tx_bit(void);
int  sp_v42_connected(void);           /* spandsp's LAPM is in its data state */
int  sp_v42_seen_connected(void);      /* status callbacks so far */
int  sp_v42_seen_disconnected(void);
void sp_v42_stop(void);                /* spandsp's v42_stop(): it sends DISC */

/* ---- spandsp V.42 bis on its own. mode: 0 dynamic, 1 always, 2 never. */
int  sp_v42bis_open(int p1, int p2, int mode, sp_put_fn enc_out, sp_put_fn dec_out, void *user);
int  sp_v42bis_compress(const uint8_t *buf, int len);
int  sp_v42bis_compress_flush(void);
int  sp_v42bis_decompress(const uint8_t *buf, int len);
int  sp_v42bis_decompress_flush(void);

#endif
