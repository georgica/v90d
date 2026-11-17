/* Telephony layer: see tel.h. */
#include <string.h>
#include "tel.h"
#include "../modem/sintab.h"

#define MS(x) ((int32_t)(x) * (TEL_FS / 1000))

/* hook timing */
#define OFFHOOK_MS     50       /* loop closed this long: a call begins */
#define PULSE_MIN_MS   10       /* a break between these two is a dial pulse */
#define PULSE_MAX_MS   150
#define HANGUP_MS      250      /* a break this long is the end of the call */
#define DIGIT_GAP_MS   200      /* loop closed this long after pulses: the digit is complete */

/* dialling */
#define DIALTONE_MAX_MS   15000 /* no digit: congestion tone */
#define PARTIAL_MAX_MS    20000 /* an incomplete number */
#define WRONG_WAIT_MS     2000  /* quiet after a number that cannot be ours */
#define ANY_WAIT_MS       3000  /* "any number": the number is over after this pause */
#define DELAY_MIN_MS      500   /* post-dial delay, drawn per call */
#define DELAY_MAX_MS      3000
#define ENDED_QUIET_MS    1000  /* silence after a call before busy tone */

typedef struct { int f1, f2, on_ms, off_ms; } tone_t;   /* off_ms 0 = continuous */
enum { T_DIAL, T_RING, T_BUSY, T_CONG };
static const tone_t tones[2][4] = {
    { { 350, 440, 0, 0 }, { 440, 480, 2000, 4000 }, { 480, 620, 500, 500 }, { 480, 620, 250, 250 } },
    { { 425, 0, 0, 0 },   { 425, 0, 1000, 4000 },   { 425, 0, 500, 500 },   { 425, 0, 250, 250 } }
};
/* special information tones before the congestion tone of a wrong number */
static const int sit_f[3] = { 950, 1400, 1800 };
#define SIT_MS      330
#define SIT_LEAD_MS 500

static uint32_t rnd(tel_t *t)
{
    uint32_t x = t->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return t->rng = x;
}

static int level(const tel_t *t) { return t->cfg.tone_level > 0 ? t->cfg.tone_level : 2000; }

/* one sample of f1 (+ f2) */
static int16_t osc(tel_t *t, int f1, int f2)
{
    int32_t v;
    /* 2^32 / 8000 = 536870.912 */
    t->ph[0] += (uint32_t)f1 * 536871u;
    v = (int32_t)sintab[t->ph[0] >> 22] * level(t) >> 15;
    if (f2) {
        t->ph[1] += (uint32_t)f2 * 536871u;
        v += (int32_t)sintab[t->ph[1] >> 22] * level(t) >> 15;
    }
    return (int16_t)v;
}

/* cadenced tone; at = samples since the tone began */
static int16_t cadence(tel_t *t, const tone_t *tn, int32_t at)
{
    if (tn->off_ms) {
        int32_t per = MS(tn->on_ms + tn->off_ms), in = at % per;
        if (in == 0) t->ph[0] = t->ph[1] = 0;
        if (in >= MS(tn->on_ms)) return 0;
    }
    return osc(t, tn->f1, tn->f2);
}

/* ---- DTMF receiver ---- */
/* 2 cos(2 pi f / 8000) in Q14 for 697 770 852 941 1209 1336 1477 1633 Hz */
static const int32_t dt_coef[8] = { 27980, 26956, 25701, 24219, 19073, 16325, 13085, 9315 };
static const char dt_keys[] = "123A456B789C*0#D";

void tel_dtmf_init(tel_dtmf *d) { memset(d, 0, sizeof *d); d->last = -1; }

/* verdict on a finished block: key index 0..15 or -1; *pw = its tone energy */
static int dt_block(tel_dtmf *d, int64_t *pw)
{
    int64_t m[8], tot; int i, r = 0, c = 4;
    for (i = 0; i < 8; i++) {
        int64_t a = d->q1[i], b = d->q2[i];
        m[i] = a * a + b * b - ((a * b) >> 14) * dt_coef[i];
        if (m[i] < 0) m[i] = 0;
    }
    for (i = 1; i < 4; i++) if (m[i] > m[r]) r = i;
    for (i = 5; i < 8; i++) if (m[i] > m[c]) c = i;
    *pw = m[r] + m[c];
    /* loud enough: mean square of the block above about amplitude 100 */
    if (d->energy < (int64_t)5000 * TEL_DTMF_N) return -1;
    /* twist within 8 dB */
    if (m[r] > m[c] * 6 || m[c] > m[r] * 6) return -1;
    /* one row and one column stand out by 6 dB */
    for (i = 0; i < 4; i++) if (i != r && m[i] * 4 > m[r]) return -1;
    for (i = 4; i < 8; i++) if (i != c && m[i] * 4 > m[c]) return -1;
    /* and hold half of all the energy: for two sines alone
     * m[r] + m[c] = (N / 2) * energy */
    tot = d->energy * (TEL_DTMF_N / 2);
    if ((m[r] + m[c]) * 2 < tot) return -1;
    return r * 4 + (c - 4);
}

int tel_dtmf_rx(tel_dtmf *d, int16_t x)
{
    int i, key, out = 0; int64_t pw;
    for (i = 0; i < 8; i++) {
        int32_t q0 = x + (int32_t)(((int64_t)dt_coef[i] * d->q1[i]) >> 14) - d->q2[i];
        d->q2[i] = d->q1[i]; d->q1[i] = q0;
    }
    d->energy += (int32_t)x * x;
    if (++d->n < TEL_DTMF_N) return 0;
    key = dt_block(d, &pw);
    /* a key counts from the third block in a row of steady strength (a block
     * the tone only partly fills is weaker and starts the count again) */
    if (key >= 0 && key == d->last && pw * 5 >= d->q_pw * 2 && d->q_pw * 5 >= pw * 2) d->run++;
    else d->run = key >= 0 ? 1 : 0;
    if (key < 0 || key != d->last) {
        if (key < 0 && d->last < 0) d->reported = 0;    /* two blocks without: the key is up */
    }
    if (key >= 0 && d->run >= 3 && !d->reported) { d->reported = 1; out = dt_keys[key]; }
    d->last = key; d->q_pw = pw;
    for (i = 0; i < 8; i++) d->q1[i] = d->q2[i] = 0;
    d->energy = 0; d->n = 0;
    return out;
}

/* ---- the line ---- */
static void go(tel_t *t, tel_state s) { t->state = s; t->t = 0; t->ph[0] = t->ph[1] = 0; }

void tel_init(tel_t *t, const tel_cfg *cfg)
{
    memset(t, 0, sizeof *t);
    t->cfg = *cfg;
    t->cfg.number[TEL_NUMBER_MAX] = 0;
    t->rng = cfg->seed ? cfg->seed : 0x2545F491u;
    tel_dtmf_init(&t->dtmf);
}

void tel_reconfigure(tel_t *t, const tel_cfg *cfg)
{
    if (t->state == TEL_ONHOOK) {
        uint32_t seed = t->rng;
        t->cfg = *cfg; t->cfg.number[TEL_NUMBER_MAX] = 0;
        t->rng = seed;
        t->have_next = 0;
    } else {
        t->next = *cfg; t->have_next = 1;
    }
}

void tel_activity(tel_t *t) { t->idle_s = 0; }

void tel_call_ended(tel_t *t)
{
    if (t->state != TEL_ANSWERED) return;
    if (t->end == TEL_END_NONE) t->end = TEL_END_FAR;
    t->want_hangup = 0;
    go(t, TEL_ENDED);
}

static void accept_number(tel_t *t)
{
    int span = DELAY_MAX_MS - DELAY_MIN_MS;
    t->delay = MS(DELAY_MIN_MS + (int32_t)(rnd(t) % (uint32_t)(span + 1)));
    t->will_busy = (int)(rnd(t) % 100u) < t->cfg.busy_chance;
    go(t, TEL_CONNECTING);
}

/* what has been dialled so far and the numbers that connect: 2 = it is one
 * of them, 1 = it begins one, 0 = it can be none */
static int dialed_is(const tel_t *t)
{
    /* the port's own number, when it has the ten digits of a North American
     * one, is taken with the long distance 1 in front as well (what Windows
     * dials unless told otherwise) */
    char one[TEL_NUMBER_MAX + 2];
    int i, best = 0;
    one[0] = 0;
    if (strlen(t->cfg.number) == 10 && t->cfg.number[0] != '1' && TEL_NUMBER_MAX >= 11) {
        one[0] = '1'; strcpy(one + 1, t->cfg.number);
    }
    for (i = -2; i < TEL_ALSO; i++) {
        const char *num = i == -2 ? one : i < 0 ? t->cfg.number : t->cfg.also[i];
        size_t want = strlen(num);
        size_t k;
        if (!want || (size_t)t->ndialed > want) continue;
        for (k = 0; k < (size_t)t->ndialed; k++)        /* in also[], '?' is any digit */
            if (t->dialed[k] != num[k] && !(i >= 0 && num[k] == '?' && t->dialed[k] >= '0' && t->dialed[k] <= '9')) break;
        if (k < (size_t)t->ndialed) continue;
        if ((size_t)t->ndialed == want) return 2;
        best = 1;
    }
    return best;
}

static void digit(tel_t *t, char k)
{
    size_t want = strlen(t->cfg.number);
    if (t->state != TEL_DIALTONE && t->state != TEL_DIALING) return;
    if (k >= 'A' && k <= 'D') return;
    if (t->state == TEL_DIALTONE) go(t, TEL_DIALING);
    t->idle = 0;
    if (want == 0 && k == '#' && t->ndialed > 0) { accept_number(t); return; }
    if (t->ndialed >= TEL_NUMBER_MAX) { t->overflow = 1; return; }
    t->dialed[t->ndialed++] = k; t->dialed[t->ndialed] = 0;
    if (want && dialed_is(t) == 2) accept_number(t);
}

static void hook_logic(tel_t *t, int raw)
{
    raw = raw ? 1 : 0;
    if (!t->hook) {
        /* on hook: wait for a steady loop */
        t->hook_n = raw ? t->hook_n + 1 : 0;
        if (t->hook_n >= MS(OFFHOOK_MS)) {
            t->hook = 1; t->hook_n = 0;
            t->calls++;
            t->ndialed = 0; t->dialed[0] = 0; t->overflow = 0;
            t->pulses = 0; t->since_make = 0; t->idle = 0;
            t->end = TEL_END_NONE; t->want_hangup = 0;
            t->answered_this = t->was_busy = t->was_wrong = 0;
            tel_dtmf_init(&t->dtmf);
            go(t, TEL_DIALTONE);
        }
        return;
    }
    if (!raw) {
        t->since_make = 0;
        if (++t->hook_n >= MS(HANGUP_MS)) {
            t->hook = 0; t->hook_n = 0; t->pulses = 0;
            if (t->end == TEL_END_NONE) t->end = TEL_END_CALLER;
            t->want_hangup = 0;
            go(t, TEL_ONHOOK);
            if (t->have_next) {
                uint32_t seed = t->rng;
                t->cfg = t->next; t->cfg.number[TEL_NUMBER_MAX] = 0;
                t->rng = seed; t->have_next = 0;
            }
        }
        return;
    }
    if (t->hook_n) {
        /* a break just ended */
        if (t->hook_n >= MS(PULSE_MIN_MS) && t->hook_n <= MS(PULSE_MAX_MS) &&
            (t->state == TEL_DIALTONE || t->state == TEL_DIALING))
            t->pulses++;
        t->hook_n = 0;
    }
    if (t->pulses && ++t->since_make >= MS(DIGIT_GAP_MS)) {
        if (t->pulses <= 10) digit(t, (char)('0' + t->pulses % 10));
        t->pulses = 0; t->since_make = 0;
    }
}

/* once a second while the call is up */
static void call_second(tel_t *t)
{
    t->call_s++; t->idle_s++;
    if (t->cfg.drop_mean_min > 0) {
        /* the same chance every second: call lengths come out exponential */
        uint32_t p = 0xFFFFFFFFu / ((uint32_t)t->cfg.drop_mean_min * 60u);
        if (rnd(t) < p) {
            if (t->end == TEL_END_NONE) t->end = TEL_END_DROP;
            t->want_hangup = 0;
            go(t, TEL_ENDED);
            return;
        }
    }
    if (t->want_hangup) return;
    if (t->cfg.session_limit_min > 0 && t->call_s >= (uint32_t)t->cfg.session_limit_min * 60u) {
        t->want_hangup = 1; t->end = TEL_END_LIMIT;
    } else if (t->cfg.idle_timeout_min > 0 && t->idle_s >= (uint32_t)t->cfg.idle_timeout_min * 60u) {
        t->want_hangup = 1; t->end = TEL_END_IDLE;
    }
}

int16_t tel_tick(tel_t *t, int offhook, int16_t rx)
{
    const tone_t *tn = tones[t->cfg.tones == TEL_TONES_EU ? 1 : 0];
    int16_t out = 0;
    int32_t at;

    hook_logic(t, offhook);
    if (t->state == TEL_ONHOOK) return 0;

    if ((t->state == TEL_DIALTONE || t->state == TEL_DIALING) && offhook) {
        int k = tel_dtmf_rx(&t->dtmf, rx);
        if (k) digit(t, (char)k);
    }

    at = t->t;
    switch (t->state) {
    case TEL_DIALTONE:
        out = cadence(t, &tn[T_DIAL], at);
        if (at >= MS(DIALTONE_MAX_MS)) go(t, TEL_TIMEOUT);
        break;
    case TEL_DIALING: {
        size_t want = strlen(t->cfg.number);
        int wrong = want && (t->overflow || !dialed_is(t));
        t->idle++;
        if (t->pulses) t->idle = 0;
        if (want == 0) { if (t->idle >= MS(ANY_WAIT_MS)) accept_number(t); }
        else if (wrong) { if (t->idle >= MS(WRONG_WAIT_MS)) { t->was_wrong = 1; go(t, TEL_INTERCEPT); } }
        else if (t->idle >= MS(PARTIAL_MAX_MS)) go(t, TEL_TIMEOUT);
        break;
    }
    case TEL_CONNECTING: {
        const tone_t *r = &tn[T_RING];
        int32_t ring_at = at - t->delay;
        if (ring_at < 0) break;
        if (t->will_busy) { t->was_busy = 1; go(t, TEL_BUSY); break; }
        if (t->cfg.rings <= 0 ||
            ring_at >= (int32_t)t->cfg.rings * MS(r->on_ms + r->off_ms) - MS(r->off_ms)) {
            t->answered++; t->answered_this = 1;
            t->call_s = t->idle_s = 0; t->sec = 0;
            go(t, TEL_ANSWERED);
            break;
        }
        out = cadence(t, r, ring_at);
        break;
    }
    case TEL_ANSWERED:
        if (++t->sec >= TEL_FS) { t->sec = 0; call_second(t); }
        return 0;                       /* t->t is not needed here and may have been reset */
    case TEL_BUSY:
        out = cadence(t, &tn[T_BUSY], at);
        break;
    case TEL_INTERCEPT: {
        int32_t a = at - MS(SIT_LEAD_MS);
        if (a < 0) break;
        if (a < 3 * MS(SIT_MS)) {
            if (a % MS(SIT_MS) == 0) t->ph[0] = 0;
            out = osc(t, sit_f[a / MS(SIT_MS)], 0);
        } else if (a >= 3 * MS(SIT_MS) + MS(SIT_LEAD_MS))
            out = cadence(t, &tn[T_CONG], a - 3 * MS(SIT_MS) - MS(SIT_LEAD_MS));
        break;
    }
    case TEL_TIMEOUT:
        out = cadence(t, &tn[T_CONG], at);
        break;
    case TEL_ENDED:
        if (at >= MS(ENDED_QUIET_MS)) out = cadence(t, &tn[T_BUSY], at - MS(ENDED_QUIET_MS));
        break;
    default:
        break;
    }
    /* a state change above has already set t->t to 0 */
    if (t->t == at) t->t = at + 1;
    if (t->t > 0x3FFFFFFF) t->t -= MS(120000);   /* a multiple of every cadence period */
    return out;
}
