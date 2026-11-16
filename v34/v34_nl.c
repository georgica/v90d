/* v90d/v34 - non-linear encoder, V.34 9.7, and its inverse.
 *
 *   x'(n) = Phi(n) x(n),  Phi = 1 + zeta / 6 + zeta^2 / 120,
 *   zeta = Theta |x(n)|^2 / (average energy of x),  Theta = 0.3125
 *
 * The "average energy of the signal x(n)" is taken as the nominal mean
 * energy of the data constellation (v34_params_t.energy_q8), a constant
 * both ends can compute; see README.md. */
#include "v34.h"

/* Phi in Q16 for the energy e (Q14 units^2) of a point, the mean being energy_q8 */
int64_t v34_nl_phi_q16(uint32_t energy_q8, int64_t e)
{
    /* zeta = (5 / 16) e / (energy_q8 * 2^6), in Q16 */
    int64_t z = (e * 320) / (int64_t)energy_q8;
    if (z > 8 * 65536) z = 8 * 65536;
    return 65536 + z / 6 + ((z * z) >> 16) / 120;
}

static int64_t phi_q16(const v34_params_t *p, int64_t e) { return v34_nl_phi_q16(p->energy_q8, e); }

static int16_t sat(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static int64_t rsh16(int64_t v) { return (v + 32768) >> 16; }

v34_pt_t v34_nl_encode(const v34_params_t *p, v34_pt_t x)
{
    v34_pt_t y = x;
    if (p->cfg.nonlinear) {
        int64_t phi = phi_q16(p, (int64_t)x.re * x.re + (int64_t)x.im * x.im);
        y.re = sat(rsh16(x.re * phi)); y.im = sat(rsh16(x.im * phi));
    }
    return y;
}

v34_pt_t v34_nl_decode(const v34_params_t *p, v34_pt_t xp)
{
    v34_pt_t y = xp;
    if (p->cfg.nonlinear) {
        /* |x'|^2 = |x|^2 Phi(|x|^2)^2: fixed-point iteration on |x|^2, which
         * closes the error by a factor of three or more each round for any
         * point a constellation holds (up to four times the mean energy) */
        int64_t ep = (int64_t)xp.re * xp.re + (int64_t)xp.im * xp.im, e = ep, phi;
        int k;
        for (k = 0; k < 8; k++) {
            phi = phi_q16(p, e);
            e = (ep << 28) / ((phi * phi) >> 4);
        }
        phi = phi_q16(p, e);
        y.re = sat((((int64_t)xp.re << 17) / phi + (xp.re < 0 ? -1 : 1)) / 2);
        y.im = sat((((int64_t)xp.im << 17) / phi + (xp.im < 0 ? -1 : 1)) / 2);
    }
    return y;
}
