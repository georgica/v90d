/* V.90 PSTN channel simulator (x86 test peer): the two-port path between our
 * DIGITAL (server) modem, which speaks 8-bit mu-law codewords on the network
 * PCM highway, and the ANALOG client modem, which speaks linear samples.
 *
 * Downstream (server -> client): codeword -> optional digital pad -> client
 *   codec D/A level -> subscriber-loop filter -> + AWGN -> client receives.
 * Upstream (client -> server): client analog -> loop filter -> + hybrid echo of
 *   the downstream line signal -> + AWGN -> network A/D (mu-law) -> server.
 *
 * Clocks are matched here (network rate); the clock-offset stage (resamp) is
 * validated separately and inserted when the modem's timing recovery is tested.
 */
#ifndef V90D_SIM_CHANNEL_H
#define V90D_SIM_CHANNEL_H
#include <stdint.h>
#include "filt.h"
#include "noise.h"

#define CH_MAXTAPS  64
#define CH_MAXDELAY 1024

typedef struct {
    double        ds_pad_db;     /* downstream digital pad attenuation (dB) */
    double        ds_noise_dbfs; /* downstream AWGN level (use <= -200 for none) */
    const double *ds_taps; int ds_ntaps;   /* downstream loop filter */
    double        us_noise_dbfs; /* upstream AWGN level */
    const double *us_taps; int us_ntaps;   /* upstream loop filter */
    double        echo_gain;     /* downstream->upstream hybrid leak (linear) */
    int           echo_delay;    /* echo delay in samples (0..CH_MAXDELAY-1) */
    double        full_scale;    /* dBFS reference amplitude (e.g. 32124) */
    uint32_t      seed;
} channel_cfg;

typedef struct {
    channel_cfg cfg;
    double  ds_pad_lin;
    fir_t   ds_loop, us_loop;
    double  ds_z[CH_MAXTAPS], us_z[CH_MAXTAPS];
    double  echo_ring[CH_MAXDELAY];
    int     echo_pos;
    rng_t   rng;
    int16_t us_lin;      /* the last upstream sample as a 16-bit linear A/D would give it
                            (the value the mu-law encoder was fed) */
} channel_t;

void channel_init(channel_t *c, const channel_cfg *cfg);

/* Advance one network-rate sample. ds_code = codeword the server transmits
 * downstream; us_analog = sample the client transmits upstream. Writes the
 * client's received analog sample and the codeword the server receives. */
void channel_tick(channel_t *c, uint8_t ds_code, double us_analog,
                  double *client_rx, uint8_t *server_rx);

#endif
