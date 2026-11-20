/* The sample pump of the line driver: what sits between the TDM interrupt
 * and the daemon's 10 ms blocks (srv/line.h, "dev:").
 *
 * Interrupt side: pump_tick() takes the n samples just received and gives
 * the n codewords to send. Process side: pump_read() hands out whole blocks
 * of 80 received samples, pump_write() queues the daemon's answers.
 *
 * Two formats of the receive direction (the transmit direction is G.711
 * codewords in both, because V.90 and V.92 put exact G.711 levels on the
 * line):
 *   companded  the codec runs in G.711 both ways; a received sample is a
 *              codeword (0..255 in the int16_t the pump carries)
 *   linear     the codec runs in 16-bit linear both ways; a received sample
 *              is the codec's 16-bit value, and each codeword to send is
 *              turned into the exact linear value of that codeword
 *              (pump_lin16, the 16-bit scale of V.90 Table 1: mu-law up to
 *              +/-32124, A-law up to +/-32256) on its way to the codec.
 * The pump itself only moves the samples; the helpers below do the two
 * conversions at its edges (FIFO word <-> sample, block -> octets of the
 * device), so that they are tested on the host with it.
 *
 * What it guarantees: every codeword written in time is sent exactly once,
 * in order, and the delay from a block read to the first codeword of its
 * answer on the wire is constant (the latency given at start) for as long as
 * the answers arrive in time. When one is late the pump sends idle codewords
 * in its place and counts them, and the late codewords whose place has
 * passed are dropped: the delay never changes during a call (the modem's
 * echo canceller and the V.90 frame alignment both rest on that).
 *
 * No allocation, no floating point; builds in the kernel and on the host.
 * The caller serialises: the driver holds its lock (interrupts off) around
 * every call. */
#ifndef V90D_FW_PUMP_H
#define V90D_FW_PUMP_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define PUMP_BLOCK 80
#define PUMP_RING  1024             /* a power of two, codewords each way */

typedef struct {
    int16_t  rx[PUMP_RING];             /* codewords (0..255) or linear samples */
    uint8_t  tx[PUMP_RING];             /* codewords */
    unsigned rx_w, rx_r, tx_w, tx_r;    /* free-running counts */
    uint8_t  idle;                      /* the codeword of silence */
    unsigned long underruns;            /* idle codewords sent for want of an answer */
    unsigned long overruns;             /* received codewords dropped, nobody read */
} pump_t;

/* empty both ways, then `latency` idle codewords queued to send */
void pump_start(pump_t *p, uint8_t idle, unsigned latency);
void pump_tick(pump_t *p, const int16_t *rx, uint8_t *tx, unsigned n);
int  pump_ready(const pump_t *p);                       /* a block can be read */
int  pump_read(pump_t *p, int16_t out[PUMP_BLOCK]);     /* 1, or 0: not yet */
int  pump_write(pump_t *p, const uint8_t in[PUMP_BLOCK]); /* 0, or -1: full */

/* ---- the two formats ---- */

/* The linear value of a G.711 codeword on the 16-bit scale: for mu-law
 * (2s + 33) * 2^(c + 2) - 132 with chord c and step s of the inverted
 * codeword, for A-law (2s + 1) * 8 in chord 0 and (2s + 33) * 2^(c + 2)
 * above (even bits inverted on the wire); bit 7 set is positive. The same
 * numbers as libv90modem's v90/v90_pcm.c (test_pump checks all 512). */
int16_t  pump_lin16(int alaw, uint8_t cw);
void     pump_lin16_table(int alaw, int16_t t[256]);

/* A word of the TDM FIFO (one slot of 16 bits in bits 31:16, first bit on
 * the wire = bit 31). lin = the table of the port's law when the codec runs
 * linear, 0 when it runs companded (the codeword is the first octet of the
 * slot, the second is ones). */
uint32_t pump_tx_word(const int16_t *lin, uint8_t cw);
int16_t  pump_rx_sample(int linear, uint32_t word);

/* What one read of the device gives: the hook state, then the 80 samples of
 * a block, as 80 octets (companded: PUMP_READ_COMPANDED octets in all) or as
 * 80 little-endian 16-bit values (linear: PUMP_READ_LINEAR). The length
 * tells the reader which. Returns the length. */
#define PUMP_READ_COMPANDED (1 + PUMP_BLOCK)
#define PUMP_READ_LINEAR    (1 + 2 * PUMP_BLOCK)
unsigned pump_block_out(int hook, int linear, const int16_t s[PUMP_BLOCK], uint8_t out[PUMP_READ_LINEAR]);

/* A level meter on the received samples as linear values (the caller
 * converts codewords first), for the driver's statistics: mean, peak and
 * mean square over the last PUMP_METER samples (1.024 s). It is how the
 * two formats are compared on the unit. */
#define PUMP_METER 8192
typedef struct {
    long     acc; unsigned long long pow; unsigned peak, n;
    long     dc; unsigned long ms; unsigned last_peak;  /* of the last whole period */
} pump_meter;
void pump_meter_add(pump_meter *m, int16_t v);

#endif
