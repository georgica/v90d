/* libv90modem: a software voice-band data modem (V.92, V.90, V.34, V.32bis,
 * V.22bis, V.22, V.21, V.8) with V.42 error control (LAPM, V.42bis, V.44).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The API is NOT stable before version 1.0: any 0.x release may change it.
 *
 * Rules the library keeps: C99, fixed-point arithmetic in the signal path, no
 * memory allocation after v90m_create() / v90m_ec_create(), no global state
 * (any number of modems in one process, one thread per object at a time).
 *
 * The line: 8000 samples per second, at a G.711 codec. The digital (V.90 /
 * V.92 server) side needs the codec's exact codewords on the line, so the
 * modem sends codewords, not linear samples, wherever a PCM modulation may
 * be offered.
 *
 * Data: the modem is a synchronous bit pump. Once connected, it calls getbit
 * for every bit it transmits and putbit for every bit it receives, from
 * inside v90m_process(), at the moment the bit is needed or decided. V.42
 * (below) or a start-stop framer sits on that pump. Callbacks are only ever
 * made from inside v90m_process() and v90m_ec_*() on the calling thread. */
#ifndef V90MODEM_H
#define V90MODEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V90M_VERSION_MAJOR 0
#define V90M_VERSION_MINOR 1
#define V90M_VERSION_PATCH 0
/* "0.1.0": the version of the library linked in (may differ from the header) */
const char *v90m_version(void);

/* error codes (negative returns) */
#define V90M_OK        0
#define V90M_EINVAL   -1    /* a configuration value out of range */
#define V90M_ENOTSUP  -2    /* not implemented in this version (the originating role, for now) */
#define V90M_ENOMEM   -3    /* create could not allocate, or init was given too little memory */
#define V90M_ESTATE   -4    /* not possible in the present state of the call */

/* ------------------------------------------------------------------ G.711
 * Codeword <-> linear on the scale of V.90 Table 1 (16-bit, the scale the
 * modem works in). alaw: 0 = mu-law, 1 = A-law. */
uint8_t v90m_g711_encode(int alaw, int16_t linear);    /* nearest codeword */
int16_t v90m_g711_decode(int alaw, uint8_t codeword);

/* ------------------------------------------------------------------ modem */
typedef struct v90m v90m_t;

typedef enum {
    V90M_ROLE_ANSWER = 0,       /* the called modem */
    V90M_ROLE_ORIGINATE = 1     /* the calling modem: V90M_ENOTSUP in 0.x until it exists */
} v90m_role;

/* modulations offered (v90m_cfg.modulations) */
#define V90M_MOD_V21     0x0001u
#define V90M_MOD_V22     0x0002u     /* V.22 and V.22bis */
#define V90M_MOD_V32BIS  0x0004u     /* V.32bis and V.32 */
#define V90M_MOD_V34     0x0008u
#define V90M_MOD_V90     0x0010u     /* the digital (server) side: answering only */
#define V90M_MOD_V92     0x0020u     /* with V90M_MOD_V90: PCM upstream on offer */

/* the modulation of a connection (v90m_state.modulation) */
typedef enum {
    V90M_CONN_NONE = 0, V90M_CONN_V21, V90M_CONN_V22, V90M_CONN_V22BIS,
    V90M_CONN_V90, V90M_CONN_V34, V90M_CONN_V32BIS, V90M_CONN_V92
} v90m_conn;

typedef int  (*v90m_getbit_fn)(void *user);             /* next bit to send, 0 or 1 */
typedef void (*v90m_putbit_fn)(void *user, int bit);    /* a bit received */

/* State changes, reported from inside v90m_process() right after the sample
 * at which they happened, before the next one is processed. */
typedef enum {
    V90M_EV_CONNECTED = 1,      /* data mode reached: v90m_state has the modulation and rates */
    V90M_EV_RATES,              /* the rates or the modulation changed (renegotiation, retrain) */
    V90M_EV_DATA_STOPPED,       /* data stopped moving (retrain, rate renegotiation) */
    V90M_EV_DATA_MOVING,        /* data moves again both ways (error control: say so to V.42) */
    V90M_EV_ENDED               /* the modem stopped: see v90m_state.end */
} v90m_event;
typedef void (*v90m_event_fn)(void *user, v90m_event ev);

typedef struct {
    v90m_role      role;
    unsigned       modulations;     /* V90M_MOD_*; default all but V.92 */
    int            alaw;            /* the codec's law: 0 mu-law, 1 A-law */
    int            codec_linear_rx; /* 1: the codec receives in 16-bit linear, with no G.711
                                       quantiser on the receive path (V.92 then designs the
                                       upstream constellation without its share of the noise).
                                       0: the received samples went through G.711, also when
                                       they are handed over as linear values */
    int            tx_level;        /* transmit RMS outside PCM modes, 16-bit units; default 3000 */
    int            max_tx_code;     /* power ceiling: the most downstream power the caller may ask for,
                                       -0.5 dBm0 less 0.5 dB per step, 1..31; 0 = 23 (-12 dBm0) */
    int            max_down_bps;    /* rate ceilings, bit/s; 0 = none. V.34: max_down caps what we */
    int            max_up_bps;      /* send, max_up what we receive. V.92 PCM up to 48000; 33600 holds it at 33333 */
    int            echo_delay;      /* samples from a sample leaving to its echo starting back (the
                                       driver's buffering); -1 = measure it in every start-up */
    v90m_getbit_fn getbit;
    v90m_putbit_fn putbit;
    v90m_event_fn  on_event;        /* may be NULL: poll v90m_get_state() instead */
    void          *user;
} v90m_cfg;

/* Memory for v90m_init() / v90m_ec_init() / v90m_async_init() as part of a
 * struct of the caller's (nothing allocated at all): upper bounds of
 * v90m_mem_size() and the others for this version, on every platform. They
 * change with the version; the init functions check the length given. */
#define V90M_MEM_SIZE        (100u * 1024u)
#define V90M_EC_MEM_SIZE     (50u * 1024u)
#define V90M_ASYNC_MEM_SIZE  2048u
/* alignment for that memory:  union { v90m_align a; uint8_t b[V90M_MEM_SIZE]; } mem; */
typedef union { long double ld; double d; int64_t i; void *p; void (*f)(void); } v90m_align;

void     v90m_cfg_default(v90m_cfg *cfg);
/* One modem. The only allocation the library makes for it. */
v90m_t  *v90m_create(const v90m_cfg *cfg, int *err);
/* Or in memory of the caller's, at least v90m_mem_size() octets, aligned for
 * any type (nothing is allocated, v90m_destroy() is not called). */
size_t   v90m_mem_size(void);
v90m_t  *v90m_init(void *mem, size_t len, const v90m_cfg *cfg, int *err);
/* Back to the start of a new call (the line went off-hook), same memory. */
int      v90m_reset(v90m_t *m, const v90m_cfg *cfg);
void     v90m_destroy(v90m_t *m);

/* The line: n received samples in, n samples to send out. Any n, 1
 * included: the result does not depend on how the samples are cut into
 * blocks. Use one of the three for a call:
 *   _g711     codewords in, codewords out: the interface for a codec, and the
 *             one V.90 / V.92 need (the far end decodes our exact codewords)
 *   _lin_rx   a receive side in 16-bit linear (Table 1 scale: what
 *             v90m_g711_decode gives), codewords out; V.90 / V.92 work too
 *   _linear   linear both ways (a PCM modulation then sends the linear value
 *             of its codewords)
 * Return V90M_OK, or V90M_EINVAL. */
int      v90m_process_g711(v90m_t *m, const uint8_t *rx, uint8_t *tx, size_t n);
int      v90m_process_lin_rx(v90m_t *m, const int16_t *rx, uint8_t *tx, size_t n);
int      v90m_process_linear(v90m_t *m, const int16_t *rx, int16_t *tx, size_t n);

/* Ask for a retrain (V.92, V.90, V.34 data mode). V90M_OK or V90M_ESTATE. */
int      v90m_retrain(v90m_t *m);
/* Hang up, telling the far modem where the modulation has a way to (V.90
 * 9.7, V.92 9.11): V90M_OK when that has begun (V90M_EV_ENDED follows within
 * a fraction of a second); V90M_ESTATE when there is nothing to send: stop. */
int      v90m_cleardown(v90m_t *m);

typedef enum {
    V90M_END_NONE = 0,
    V90M_END_FAILED,            /* no connection was made (start-up failed) */
    V90M_END_LOST,              /* the connection was lost */
    V90M_END_CLEARDOWN_FAR,     /* the far modem cleared down */
    V90M_END_CLEARDOWN_OURS     /* after v90m_cleardown() */
} v90m_end;

/* Cheap; fine to read after every sample. */
typedef struct {
    int        connected;       /* data mode reached, and kept through retrains */
    int        moving;          /* user data moves both ways now */
    int        retraining;
    v90m_conn  modulation;      /* in force (during a retrain: the one before) */
    int        tx_bps, rx_bps;  /* our transmit / receive rate (rx = tx where symmetric) */
    int        used_v8;         /* the far modem spoke V.8 */
    int        odp_bypass;      /* V.92 9.3.1: the caller skips V.42's ODP / ADP */
    v90m_end   end;
} v90m_state;
void     v90m_get_state(const v90m_t *m, v90m_state *st);

typedef struct {
    v90m_conn  modulation;
    int        tx_bps, rx_bps;
    int        snr_db10;        /* receiver signal to error, 0.1 dB, 0..600; -1 = none */
    int        rtd;             /* round-trip delay measured in the handshake, samples; -1 = none */
    int        echo_delay;      /* in use (configured or measured), samples; -1 = none */
    unsigned   retrains;        /* begun, this call */
    unsigned   renegotiations;  /* rate renegotiations (V.90, V.92), this call */
} v90m_stats;
void     v90m_get_stats(const v90m_t *m, v90m_stats *st);

/* ------------------------------------- start-stop characters (V.14 basic)
 * Characters (1 start, 8 data LSB first, 1 stop, mark idle) on the bit pump
 * of a connected modem, without error control: pass v90m_async_getbit and
 * v90m_async_putbit as the modem's getbit / putbit with the object as user.
 * Fixed 512-octet queues each way. */
typedef struct v90m_async v90m_async_t;
v90m_async_t *v90m_async_create(int *err);
size_t        v90m_async_mem_size(void);
v90m_async_t *v90m_async_init(void *mem, size_t len, int *err);
void          v90m_async_reset(v90m_async_t *a);
void          v90m_async_destroy(v90m_async_t *a);
int           v90m_async_getbit(void *a);
void          v90m_async_putbit(void *a, int bit);
size_t        v90m_async_write(v90m_async_t *a, const uint8_t *buf, size_t n);  /* octets accepted */
size_t        v90m_async_read(v90m_async_t *a, uint8_t *buf, size_t max);       /* octets delivered */
size_t        v90m_async_tx_pending(const v90m_async_t *a);   /* queued or being sent */
unsigned long v90m_async_frame_errors(const v90m_async_t *a);

/* -------------------------------------------- V.42 error control (LAPM)
 * One endpoint, on the bit pump of a connected modem: feed it the modem's
 * bits (v90m_ec_tx_bit from getbit, v90m_ec_rx_bit from putbit), tick it
 * every millisecond, and move user octets with write / read. With V.42bis or
 * V.44 compression when both ends agree; falls back to start-stop (no error
 * control) when the far end has no V.42 and fallback is on. */
typedef struct v90m_ec v90m_ec_t;

typedef struct {
    int originator;         /* 1 = the calling end, 0 = the answering end */
    int detect;             /* run the detection phase (V.42 7.2.1) */
    int fallback;           /* when detection fails: 1 = start-stop, 0 = disconnect */
    int t400_ms;            /* detection phase timer, default 750 */
    int establish_ms;       /* answerer: give up when no SABME comes this long after detection; 0 = never */
    int comp_policy;        /* V90M_COMP_AUTO / ALWAYS / NEVER for our encoder */
    struct {
        int use_xid;            /* originator: negotiate with XID before SABME */
        int k_tx, k_rx;         /* window */
        int n401_tx, n401_rx;   /* information field, octets */
        int fcs32;
        int v42bis_tx, v42bis_rx;   /* V.42bis wanted, our send / receive direction */
        int v42bis_p1, v42bis_p2;   /* codewords (512..), string length (6..250) */
        int v44_tx, v44_rx;         /* V.44 wanted (preferred over V.42bis) */
        int v44_p1_tx, v44_p1_rx, v44_p2_tx, v44_p2_rx, v44_p3_tx, v44_p3_rx;
        int t401_ms, t403_ms, n400;
    } lapm;
} v90m_ec_cfg;
#define V90M_COMP_AUTO   0
#define V90M_COMP_ALWAYS 1
#define V90M_COMP_NEVER  2

enum { V90M_EC_DETECT = 0, V90M_EC_ESTABLISH, V90M_EC_CONNECTED, V90M_EC_ASYNC,
       V90M_EC_RELEASING, V90M_EC_DISCONNECTED };
/* why an endpoint is V90M_EC_DISCONNECTED (v90m_ec_status.reason) */
enum { V90M_EC_R_NONE = 0, V90M_EC_R_LOCAL, V90M_EC_R_PEER_DISC, V90M_EC_R_PEER_DM,
       V90M_EC_R_N400, V90M_EC_R_PROTOCOL, V90M_EC_R_NEGOTIATION,
       V90M_EC_R_NO_EC = 16, V90M_EC_R_COMPRESSION, V90M_EC_R_NO_SABME };
enum { V90M_EC_COMP_NONE = 0, V90M_EC_COMP_V42BIS, V90M_EC_COMP_V44 };

typedef struct {
    int state, reason;
    int k_tx, k_rx, n401_tx, n401_rx;
    int comp_alg;                   /* V90M_EC_COMP_*, agreed */
    int comp_tx, comp_rx;           /* agreed, per direction */
    int enc_on, dec_on;             /* our encoder / decoder in use now */
    int alg_on;                     /* V90M_EC_COMP_* of the encoder / decoder in use */
    int comp_n2, comp_n7;
    int v44_n2_tx, v44_n7_tx, v44_n8_tx, v44_n2_rx, v44_n7_rx, v44_n8_rx;
    int fcs32;
    size_t tx_queued, rx_avail;
    int unacked;
    unsigned long rx_frames, rx_frames_bad;
    unsigned long tx_i, tx_i_retx, rx_i, rx_i_dropped;
    unsigned long tx_rej, rx_rej, t401_expiries, violations;
    unsigned long comp_in, comp_out, decomp_in, decomp_out;
    unsigned long async_overruns, async_framing;
} v90m_ec_status;

void        v90m_ec_cfg_default(v90m_ec_cfg *cfg, int originator);
v90m_ec_t  *v90m_ec_create(const v90m_ec_cfg *cfg, int *err);
size_t      v90m_ec_mem_size(void);
v90m_ec_t  *v90m_ec_init(void *mem, size_t len, const v90m_ec_cfg *cfg, int *err);
int         v90m_ec_reset(v90m_ec_t *e, const v90m_ec_cfg *cfg);
void        v90m_ec_destroy(v90m_ec_t *e);

int         v90m_ec_tx_bit(v90m_ec_t *e);               /* from the modem's getbit */
void        v90m_ec_rx_bit(v90m_ec_t *e, int bit);      /* from the modem's putbit */
void        v90m_ec_tick(v90m_ec_t *e, unsigned ms);    /* timers */
void        v90m_ec_line_restored(v90m_ec_t *e);        /* on V90M_EV_DATA_MOVING */

size_t      v90m_ec_write(v90m_ec_t *e, const uint8_t *data, size_t len);
size_t      v90m_ec_write_space(const v90m_ec_t *e);
size_t      v90m_ec_read(v90m_ec_t *e, uint8_t *data, size_t max);
size_t      v90m_ec_read_avail(const v90m_ec_t *e);
int         v90m_ec_tx_idle(const v90m_ec_t *e);        /* everything written was acknowledged */
int         v90m_ec_tx_busy(const v90m_ec_t *e);        /* a frame is still leaving the transmitter */

void        v90m_ec_release(v90m_ec_t *e);              /* orderly: DISC, wait for UA */
void        v90m_ec_abort(v90m_ec_t *e);                /* the line is gone: stop without a word */
/* break signals (V.42 12.3.1): opt is one of these */
#define V90M_BRK_NONDESTRUCTIVE_INSEQ      0x00
#define V90M_BRK_NONDESTRUCTIVE_EXPEDITED  0x40
#define V90M_BRK_DESTRUCTIVE_EXPEDITED     0xC0
int         v90m_ec_send_break(v90m_ec_t *e, int opt, int len10ms);
int         v90m_ec_get_break(v90m_ec_t *e, int *opt, int *len10ms);

int         v90m_ec_state(const v90m_ec_t *e);
void        v90m_ec_get_status(const v90m_ec_t *e, v90m_ec_status *st);

#ifdef __cplusplus
}
#endif
#endif
