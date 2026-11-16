#include "v21.h"
#include "sintab.h"

/* phase increment per sample for frequency f: f * 2^32 / fs (integer). */
static uint32_t phase_inc(int f, int fs)
{
    return (uint32_t)(((uint64_t)(uint32_t)f << 32) / (uint32_t)fs);
}

void v21mod_init(v21mod_t *m, int f_mark, int f_space, int fs, int baud, int16_t amp)
{
    m->phase = 0;
    m->dmark = phase_inc(f_mark, fs);
    m->dspace = phase_inc(f_space, fs);
    m->baud_acc = 0;
    m->fs = fs;
    m->baud = baud;
    m->amp = amp;
}

int16_t v21mod_tick(v21mod_t *m, int bit, int *bit_done)
{
    int16_t s = sintab[(m->phase >> 22) & SIN_MASK];     /* top 10 bits -> index */
    m->phase += bit ? m->dmark : m->dspace;              /* binary 1 = mark */
    m->baud_acc += (uint32_t)m->baud;
    *bit_done = 0;
    if (m->baud_acc >= (uint32_t)m->fs) { m->baud_acc -= (uint32_t)m->fs; *bit_done = 1; }
    return (int16_t)(((int32_t)s * m->amp) >> 15);
}

int v21mod_run(v21mod_t *m, const uint8_t *bits, int nbits, int16_t *out, int maxout)
{
    int k = 0, bi = 0, done;
    while (bi < nbits && k < maxout) {
        out[k++] = v21mod_tick(m, bits[bi], &done);
        bi += done;
    }
    return k;
}

/* cos/sin from the Q15 table (cos = sin shifted a quarter period). */
static int32_t tab_sin(uint32_t ph) { return sintab[(ph >> 22) & SIN_MASK]; }
static int32_t tab_cos(uint32_t ph) { return sintab[((ph >> 22) + SIN_N / 4) & SIN_MASK]; }

static uint32_t phase_inc_d(int f, int fs)
{
    return (uint32_t)(((uint64_t)(uint32_t)f << 32) / (uint32_t)fs);
}

void v21demod_init(v21demod_t *d, int f_mark, int f_space, int fs, int baud)
{
    int i;
    d->pmark = d->pspace = 0;
    d->dmark = phase_inc_d(f_mark, fs);
    d->dspace = phase_inc_d(f_space, fs);
    d->win = fs / baud;
    if (d->win > V21_MAXWIN) d->win = V21_MAXWIN;
    for (i = 0; i < V21_MAXWIN; i++) { d->mi[i] = d->mq[i] = d->si[i] = d->sq[i] = 0; }
    d->smi = d->smq = d->ssi = d->ssq = 0;
    d->wpos = 0;
    d->baud_acc = 0;
    d->fs = fs;
    d->baud = baud;
    d->energy = 0;
}

int v21demod_tick(v21demod_t *d, int16_t xin)
{
    int32_t x = xin;
    /* quadrature products, scaled to keep the window sums in int32 */
    int32_t pmi = (x * tab_cos(d->pmark)) >> 6;
    int32_t pmq = (x * tab_sin(d->pmark)) >> 6;
    int32_t psi = (x * tab_cos(d->pspace)) >> 6;
    int32_t psq = (x * tab_sin(d->pspace)) >> 6;
    int32_t w = d->wpos;
    int64_t mp, sp;
    /* sliding boxcar: subtract oldest, add newest */
    d->smi += pmi - d->mi[w]; d->mi[w] = pmi;
    d->smq += pmq - d->mq[w]; d->mq[w] = pmq;
    d->ssi += psi - d->si[w]; d->si[w] = psi;
    d->ssq += psq - d->sq[w]; d->sq[w] = psq;
    if (++d->wpos >= d->win) d->wpos = 0;
    d->pmark += d->dmark;
    d->pspace += d->dspace;
    mp = (int64_t)d->smi * d->smi + (int64_t)d->smq * d->smq;
    sp = (int64_t)d->ssi * d->ssi + (int64_t)d->ssq * d->ssq;
    d->energy = mp + sp;
    return mp > sp;                      /* mark power dominates => binary 1 */
}

int64_t v21demod_energy_of(const v21demod_t *d, int amp)
{
    /* window sum of amp*cos^2 scaled like the tick: win * amp * 32767/2 / 64 */
    int64_t s = ((int64_t)d->win * amp * 32767) >> 7;
    return s * s;
}

int v21demod_run(v21demod_t *d, const int16_t *in, int nsamp, uint8_t *bits, int maxbits)
{
    int n, nb = 0;
    for (n = 0; n < nsamp && nb < maxbits; n++) {
        int lvl = v21demod_tick(d, in[n]);
        d->baud_acc += (uint32_t)d->baud;
        if (d->baud_acc >= (uint32_t)d->fs) {
            d->baud_acc -= (uint32_t)d->fs;
            bits[nb++] = (uint8_t)lvl;
        }
    }
    return nb;
}
