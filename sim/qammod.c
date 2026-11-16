#include <math.h>
#include "qammod.h"

#define PI 3.14159265358979323846

static double rrc(double t, double a)
{
    if (fabs(t) < 1e-9) return 1 - a + 4 * a / PI;
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return (a / sqrt(2.0)) * ((1 + 2 / PI) * sin(PI / (4 * a)) + (1 - 2 / PI) * cos(PI / (4 * a)));
    return (sin(PI * t * (1 - a)) + 4 * a * t * cos(PI * t * (1 + a))) / (PI * t * (1 - 16 * a * a * t * t));
}

void qammod_init(qammod_t *m, double sym_rate, double ppm, double fc, double gain,
                 qammod_sym_fn sym, void *user)
{
    int k;
    m->sps = 8000.0 / (sym_rate * (1 + ppm * 1e-6));
    m->fc = fc; m->alpha = 0.12; m->gain = gain;
    for (k = 0; k <= 2 * QAMMOD_SPAN; k++) m->hr[k] = m->hi[k] = 0;
    m->frac = 1.0;              /* pull the first symbol on the first tick */
    m->ph = 0; m->n = 0;
    m->sym = sym; m->user = user;
}

double qammod_delay(const qammod_t *m) { return QAMMOD_SPAN * m->sps; }

double qammod_tick(qammod_t *m)
{
    double br = 0, bi = 0, out;
    int k;
    while (m->frac >= 1.0) {
        for (k = 2 * QAMMOD_SPAN; k > 0; k--) { m->hr[k] = m->hr[k - 1]; m->hi[k] = m->hi[k - 1]; }
        m->sym(m->user, &m->hr[0], &m->hi[0]);
        m->frac -= 1.0;
    }
    /* symbol k (k back from the newest) is centred (k - SPAN + frac) symbols ago */
    for (k = 0; k <= 2 * QAMMOD_SPAN; k++) {
        double t = m->frac + k - QAMMOD_SPAN;
        double w = 0.5 + 0.5 * cos(PI * t / (QAMMOD_SPAN + 1));
        double g = rrc(t, m->alpha) * w;
        br += m->hr[k] * g; bi += m->hi[k] * g;
    }
    out = m->gain * (br * cos(m->ph) - bi * sin(m->ph));
    m->ph += 2 * PI * m->fc / 8000.0;
    if (m->ph > 2 * PI) m->ph -= 2 * PI;
    m->frac += 1.0 / m->sps;
    m->n++;
    return out;
}
