/* Telephony layer: a simulated caller (hook switch, DTMF sender, pulse
 * dialler) against tel_tick, with the tones we send measured by Goertzel. */
#include <string.h>
#include "../../test/test.h"
#include "../tel.h"

#define FS 8000
#define MS(x) ((x) * 8)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- the caller ---- */
typedef struct {
    tel_t t;
    int hook;
    double echo;            /* how much of what we send comes back */
    double noise;           /* peak of uniform noise on the line */
    uint32_t lcg;
    long n;                 /* sample clock */
    /* capture of what tel sent, most recent CAP samples */
    int16_t cap[FS * 12]; long ncap;
    int16_t last_tx;
} rigt;

static double rnd(rigt *r) { r->lcg = r->lcg * 1664525u + 1013904223u; return (double)(r->lcg >> 8) / 8388608.0 - 1.0; }

static void rig_init(rigt *r, const tel_cfg *c)
{
    memset(r, 0, sizeof *r);
    r->lcg = 12345; r->echo = 0.3; r->noise = 20;
    tel_init(&r->t, c);
}
static void step(rigt *r, double line)
{
    double x = line + r->echo * r->last_tx + r->noise * rnd(r);
    int16_t tx;
    if (x > 32767) x = 32767;
    if (x < -32768) x = -32768;
    tx = tel_tick(&r->t, r->hook, (int16_t)x);
    r->last_tx = tx;
    if (r->ncap < (long)(sizeof r->cap / sizeof r->cap[0])) r->cap[r->ncap++] = tx;
    r->n++;
}
static void run(rigt *r, int ms) { int i; for (i = 0; i < MS(ms); i++) step(r, 0); }
static void cap_reset(rigt *r) { r->ncap = 0; }

static const char keys[] = "123A456B789C*0#D";
static const double rowf[4] = { 697, 770, 852, 941 }, colf[4] = { 1209, 1336, 1477, 1633 };

/* one DTMF key: on_ms of tone, off_ms of silence */
static void key2(rigt *r, char k, int on_ms, int off_ms, double amp_lo, double amp_hi, double detune)
{
    const char *p = strchr(keys, k);
    int idx = (int)(p - keys), i;
    double fl = rowf[idx / 4] * (1 + detune), fh = colf[idx % 4] * (1 + detune);
    for (i = 0; i < MS(on_ms); i++)
        step(r, amp_lo * sin(2 * M_PI * fl * i / FS) + amp_hi * sin(2 * M_PI * fh * i / FS + 1.0));
    run(r, off_ms);
}
static void key(rigt *r, char k) { key2(r, k, 70, 70, 1500, 1900, 0); }
static void dial(rigt *r, const char *s) { for (; *s; s++) key(r, *s); }

/* loop-disconnect digit: n breaks of 60 ms, 40 ms make, then 700 ms pause */
static void pulse_digit(rigt *r, int n)
{
    int i;
    for (i = 0; i < n; i++) { r->hook = 0; run(r, 60); r->hook = 1; run(r, 40); }
    run(r, 700);
}

/* power of frequency f in the last `len` captured samples starting at a,
 * as amplitude of a sine */
static double tone_amp(const int16_t *x, long len, double f)
{
    double re = 0, im = 0; long i;
    for (i = 0; i < len; i++) { re += x[i] * cos(2 * M_PI * f * i / FS); im += x[i] * sin(2 * M_PI * f * i / FS); }
    return 2 * sqrt(re * re + im * im) / len;
}
static double rms(const int16_t *x, long len)
{
    double e = 0; long i;
    for (i = 0; i < len; i++) e += (double)x[i] * x[i];
    return sqrt(e / len);
}

static tel_cfg base_cfg(void)
{
    tel_cfg c;
    memset(&c, 0, sizeof c);
    strcpy(c.number, "5551234");
    c.rings = 2; c.seed = 7;
    return c;
}

/* run until the state is `want` or ms milliseconds pass; returns ms waited or -1 */
static int wait_state(rigt *r, tel_state want, int ms)
{
    long i;
    for (i = 0; i < (long)MS(ms); i++) { if (r->t.state == want) return (int)(i / 8); step(r, 0); }
    return r->t.state == want ? ms : -1;
}

static rigt R;

static void test_dialtone(void)
{
    tel_cfg c = base_cfg();
    rig_init(&R, &c);
    run(&R, 500);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
    CHECK(rms(R.cap, R.ncap) == 0);
    R.hook = 1; run(&R, 20);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);          /* debounce */
    run(&R, 100);
    CHECK_EQ_I(R.t.state, TEL_DIALTONE);
    cap_reset(&R); run(&R, 500);
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 350), 2000, 100);
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 440), 2000, 100);
    CHECK(tone_amp(R.cap, R.ncap, 425) < 300);
    CHECK_EQ_I(R.t.calls, 1);

    c.tones = TEL_TONES_EU; c.tone_level = 3000;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 200); cap_reset(&R); run(&R, 500);
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 425), 3000, 150);
    CHECK(tone_amp(R.cap, R.ncap, 350) < 300);
}

/* bench: the box calls (tel_cfg.originate). The modem on the port was told
 * ATA and has gone off hook: no dial tone, the call is ours after 300 ms. */
static void test_originate(void)
{
    tel_cfg c = base_cfg();
    c.originate = 1;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 120);                   /* debounce */
    CHECK(R.t.state == TEL_DIALTONE || R.t.state == TEL_ANSWERED);
    cap_reset(&R); run(&R, 400);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);
    CHECK(rms(R.cap, R.ncap) == 0);             /* no dial tone, no ringing */
    CHECK_EQ_I(R.t.answered, 1);
    R.hook = 0; run(&R, 400);                   /* the modem hangs up */
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
}

static void test_dtmf_call(void)
{
    tel_cfg c = base_cfg();
    int ms; long a;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 600);
    key(&R, '5');
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    cap_reset(&R); run(&R, 60);
    CHECK(rms(R.cap, R.ncap) == 0);             /* dial tone stops with the first digit */
    dial(&R, "551234");
    CHECK(strcmp(R.t.dialed, "5551234") == 0);
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    /* post-dial delay 0.5..3 s of silence, then ringing tone 2 s on, 4 s off, twice, then answer */
    cap_reset(&R);
    ms = wait_state(&R, TEL_ANSWERED, 20000);
    CHECK(ms >= 8000 + 500 - 200 && ms <= 8000 + 3000 + 300);
    /* find the start of the first ring burst */
    for (a = 0; a < R.ncap && R.cap[a] == 0; a++) ;
    CHECK(a >= MS(400) && a <= MS(3000));
    CHECK_NEAR(tone_amp(R.cap + a + 80, MS(1900), 440), 2000, 150);
    CHECK_NEAR(tone_amp(R.cap + a + 80, MS(1900), 480), 2000, 150);
    CHECK(rms(R.cap + a + MS(2000) + 80, MS(3900)) < 5);            /* the 4 s gap */
    CHECK_NEAR(tone_amp(R.cap + a + MS(6000) + 80, MS(1900), 440), 2000, 150);
    CHECK_EQ_I(R.t.answered, 1);
    CHECK(R.t.answered_this && !R.t.was_busy && !R.t.was_wrong);
    /* answered: we are silent, the owner runs the modem */
    cap_reset(&R); run(&R, 3000);
    CHECK(rms(R.cap, R.ncap) == 0);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);
    /* a 100 ms glitch of the hook detector does not end the call */
    R.hook = 0; run(&R, 100); R.hook = 1; run(&R, 500);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);
    /* the caller hangs up */
    R.hook = 0; run(&R, 600);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
    CHECK_EQ_I(R.t.end, TEL_END_CALLER);
    /* and can call again */
    R.hook = 1; run(&R, 300);
    CHECK_EQ_I(R.t.state, TEL_DIALTONE);
    CHECK_EQ_I(R.t.ndialed, 0);
    CHECK_EQ_I(R.t.calls, 2);
}

static void test_rings_zero_and_eu(void)
{
    tel_cfg c = base_cfg();
    int ms; long a;
    c.rings = 0;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300); dial(&R, "5551234");
    ms = wait_state(&R, TEL_ANSWERED, 5000);
    CHECK(ms >= 300 && ms <= 3300);
    c.rings = 1; c.tones = TEL_TONES_EU;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300); dial(&R, "5551234");
    cap_reset(&R);
    ms = wait_state(&R, TEL_ANSWERED, 12000);
    CHECK(ms >= 1000 + 300 && ms <= 1000 + 3300);    /* one burst of 1 s */
    for (a = 0; a < R.ncap && R.cap[a] == 0; a++) ;
    CHECK_NEAR(tone_amp(R.cap + a + 80, MS(900), 425), 2000, 150);
}

/* Windows dials a ten-digit number as long distance, with a 1 in front,
 * unless told otherwise: the port's own number takes it, as the bulletin
 * board numbers do. Not a shorter number, and not any other digit. */
static void test_trunk_one(void)
{
    tel_cfg c = base_cfg();
    strcpy(c.number, "6387825442");
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "16387825442");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK(strcmp(R.t.dialed, "16387825442") == 0);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "6387825442");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "26387825442");
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
    /* a seven-digit number has no long distance form */
    c = base_cfg();
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "15551234");
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
}

static void test_wrong_number(void)
{
    tel_cfg c = base_cfg();
    long a;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "5559999");
    cap_reset(&R);
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    CHECK(strcmp(R.t.dialed, "5559999") == 0);
    /* information tones 950, 1400, 1800 Hz, 330 ms each, then congestion tone */
    cap_reset(&R); run(&R, 8000);
    for (a = 0; a < R.ncap && R.cap[a] == 0; a++) ;
    CHECK(a < MS(1500));
    CHECK(tone_amp(R.cap + a + 80, MS(300), 950) > 1500);
    CHECK(tone_amp(R.cap + a + MS(330) + 80, MS(300), 1400) > 1500);
    CHECK(tone_amp(R.cap + a + MS(660) + 80, MS(300), 1800) > 1500);
    CHECK(tone_amp(R.cap + R.ncap - MS(2000), MS(2000), 480) > 600);   /* 50 % duty */
    CHECK(tone_amp(R.cap + R.ncap - MS(2000), MS(2000), 620) > 600);
    CHECK_EQ_I(R.t.state, TEL_INTERCEPT);
    CHECK_EQ_I(R.t.answered, 0);
    CHECK(R.t.was_wrong && !R.t.was_busy && !R.t.answered_this);
    R.hook = 0; run(&R, 600);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
    /* digits after a complete number are ignored, as exchanges do */
    R.hook = 1; run(&R, 300);
    dial(&R, "55512345");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK(strcmp(R.t.dialed, "5551234") == 0);
    R.hook = 0; run(&R, 600);
    /* too few digits time out */
    R.hook = 1; run(&R, 300);
    dial(&R, "555");
    CHECK(wait_state(&R, TEL_TIMEOUT, 25000) >= 0);
    R.hook = 0; run(&R, 600);
    /* no digit at all: dial tone for 15 s, then congestion tone */
    R.hook = 1; run(&R, 300);
    CHECK(wait_state(&R, TEL_TIMEOUT, 14000) < 0);
    CHECK(wait_state(&R, TEL_TIMEOUT, 3000) >= 0);
    cap_reset(&R); run(&R, 2000);
    CHECK(tone_amp(R.cap, R.ncap, 480) > 600);
}

/* numbers the owner knows (tel_cfg.more): asked once per digit, never per sample */
static int more_calls;
static int more_cb(void *ctx, const char *d)
{
    (void)ctx; more_calls++;
    return strcmp(d, "776") == 0 ? 2 : strncmp("776", d, strlen(d)) == 0 ? 1 : 0;
}
static void test_more(void)
{
    tel_cfg c = base_cfg();
    c.more = more_cb; c.more_ctx = NULL;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    more_calls = 0;
    dial(&R, "77");
    run(&R, 2000);                                  /* two seconds of samples while it waits */
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    CHECK_EQ_I(more_calls, 2);
    dial(&R, "6");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK_EQ_I(more_calls, 3);
    R.hook = 0; run(&R, 600);
    /* a digit it does not begin: wrong, as for any number */
    R.hook = 1; run(&R, 300);
    dial(&R, "70");
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
}

/* numbers that answer beside the port's own (tel_cfg.also) */
static void test_also(void)
{
    tel_cfg c = base_cfg();
    strcpy(c.also[0], "8002273425"); strcpy(c.also[1], "18002273425");
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "8002273425");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK(strcmp(R.t.dialed, "8002273425") == 0);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "18002273425");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    R.hook = 0; run(&R, 600);
    /* the port's own number as before */
    R.hook = 1; run(&R, 300);
    dial(&R, "5551234");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    R.hook = 0; run(&R, 600);
    /* part of one of them is not yet wrong; one digit off is */
    R.hook = 1; run(&R, 300);
    dial(&R, "180022");
    run(&R, 3000);
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "8002273426");
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
    /* '?' stands for any digit: a whole range of numbers connects */
    strcpy(c.also[1], "227???????");
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "2275550123");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK(strcmp(R.t.dialed, "2275550123") == 0);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "22755501");
    run(&R, 3000);
    CHECK_EQ_I(R.t.state, TEL_DIALING);                 /* not complete yet */
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "227*");                                   /* a digit, not any key */
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "2285550123");
    CHECK(wait_state(&R, TEL_INTERCEPT, 4000) >= 0);
    R.hook = 0; run(&R, 600);
}

static void test_any_number(void)
{
    tel_cfg c = base_cfg();
    c.number[0] = 0; c.rings = 0;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "18005551212");
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    CHECK(wait_state(&R, TEL_CONNECTING, 2500) < 0);     /* waits 3 s for more digits */
    CHECK(wait_state(&R, TEL_ANSWERED, 6000) >= 0);
    CHECK(strcmp(R.t.dialed, "18005551212") == 0);
    R.hook = 0; run(&R, 600);
    /* a number of 20 digits is kept, the 21st is counted as overflow but the call goes on */
    R.hook = 1; run(&R, 300);
    dial(&R, "123456789012345678901");
    CHECK_EQ_I(R.t.ndialed, 20);
    CHECK_EQ_I(R.t.overflow, 1);
    CHECK(wait_state(&R, TEL_ANSWERED, 9000) >= 0);
    R.hook = 0; run(&R, 600);
    /* a number the owner knows (tel_cfg.more) ends the dialling at once,
     * any other still waits its 3 s (a WebTV port: srv.c) */
    c.more = more_cb; c.more_ctx = NULL;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300);
    dial(&R, "776");
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    R.hook = 0; run(&R, 600);
    R.hook = 1; run(&R, 300);
    dial(&R, "77");
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    CHECK(wait_state(&R, TEL_ANSWERED, 6000) >= 0);
    CHECK(strcmp(R.t.dialed, "77") == 0);
}

static void test_pulse(void)
{
    tel_cfg c = base_cfg();
    strcpy(c.number, "2105");
    rig_init(&R, &c);
    R.hook = 1; run(&R, 600);
    pulse_digit(&R, 2);
    CHECK_EQ_I(R.t.state, TEL_DIALING);
    CHECK(strcmp(R.t.dialed, "2") == 0);
    pulse_digit(&R, 1); pulse_digit(&R, 10); pulse_digit(&R, 5);
    CHECK(strcmp(R.t.dialed, "2105") == 0);
    CHECK_EQ_I(R.t.state, TEL_CONNECTING);
    CHECK_EQ_I(R.t.calls, 1);                   /* the breaks were not new calls */
}

static void test_busy(void)
{
    tel_cfg c = base_cfg();
    int i, busy = 0;
    c.busy_chance = 100;
    rig_init(&R, &c);
    R.hook = 1; run(&R, 300); dial(&R, "5551234");
    CHECK(wait_state(&R, TEL_BUSY, 4000) >= 0);
    cap_reset(&R); run(&R, 4000);
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 480), 1000, 150);    /* 0.5 s on, 0.5 s off */
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 620), 1000, 150);
    CHECK(rms(R.cap + MS(20), MS(460)) > 1500);             /* the cadence itself */
    CHECK(rms(R.cap + MS(520), MS(460)) == 0);
    CHECK(rms(R.cap + MS(1020), MS(460)) > 1500);
    CHECK_EQ_I(R.t.state, TEL_BUSY);
    CHECK_EQ_I(R.t.answered, 0);
    CHECK(R.t.was_busy && !R.t.was_wrong && !R.t.answered_this);
    R.hook = 0; run(&R, 600);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
    /* 30 %: about 30 calls in 100 */
    c.busy_chance = 30; c.rings = 0;
    rig_init(&R, &c); R.noise = 0;
    for (i = 0; i < 100; i++) {
        R.hook = 1; run(&R, 100); dial(&R, "5551234"); run(&R, 3200);
        if (R.t.state == TEL_BUSY) busy++;
        else CHECK_EQ_I(R.t.state, TEL_ANSWERED);
        R.hook = 0; run(&R, 400);
    }
    printf("  busy_chance 30: %d of 100 calls busy\n", busy);
    CHECK(busy >= 18 && busy <= 42);
}

static void answer(rigt *r, const tel_cfg *c)
{
    rig_init(r, c); r->noise = 0;
    r->hook = 1; run(r, 100); dial(r, c->number);
    wait_state(r, TEL_ANSWERED, 20000);
}

static void test_call_end(void)
{
    tel_cfg c = base_cfg();
    c.rings = 0;
    answer(&R, &c);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);
    tel_call_ended(&R.t);
    CHECK_EQ_I(R.t.state, TEL_ENDED);
    CHECK_EQ_I(R.t.end, TEL_END_FAR);
    cap_reset(&R); run(&R, 900);
    CHECK(rms(R.cap, R.ncap) == 0);
    run(&R, 1000); cap_reset(&R); run(&R, 4000);
    CHECK_NEAR(tone_amp(R.cap, R.ncap, 480), 1000, 150);
    R.hook = 0; run(&R, 600);
    CHECK_EQ_I(R.t.state, TEL_ONHOOK);
    CHECK_EQ_I(R.t.end, TEL_END_FAR);           /* the first cause is kept */
}

static void test_limits(void)
{
    tel_cfg c = base_cfg();
    long i, n; double sum = 0; int k, trials = 200;
    c.rings = 0;
    /* session limit: 1 minute, to the second */
    c.session_limit_min = 1;
    answer(&R, &c);
    for (i = 0; i < 59L * FS; i++) tel_tick(&R.t, 1, 0);
    CHECK_EQ_I(R.t.want_hangup, 0);
    for (i = 0; i < 2L * FS; i++) tel_tick(&R.t, 1, 0);
    CHECK_EQ_I(R.t.want_hangup, 1);
    CHECK_EQ_I(R.t.end, TEL_END_LIMIT);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);        /* the owner closes the call */
    tel_call_ended(&R.t);
    CHECK_EQ_I(R.t.state, TEL_ENDED);
    CHECK_EQ_I(R.t.end, TEL_END_LIMIT);
    /* idle timeout: activity keeps the call */
    c.session_limit_min = 0; c.idle_timeout_min = 1;
    answer(&R, &c);
    for (k = 0; k < 5; k++) {
        for (i = 0; i < 50L * FS; i++) tel_tick(&R.t, 1, 0);
        CHECK_EQ_I(R.t.want_hangup, 0);
        tel_activity(&R.t);
    }
    for (i = 0; i < 61L * FS; i++) tel_tick(&R.t, 1, 0);
    CHECK_EQ_I(R.t.want_hangup, 1);
    CHECK_EQ_I(R.t.end, TEL_END_IDLE);
    /* no limits: an hour passes */
    c.idle_timeout_min = 0;
    answer(&R, &c);
    for (i = 0; i < 3600L * FS; i++) tel_tick(&R.t, 1, 0);
    CHECK_EQ_I(R.t.state, TEL_ANSWERED);
    CHECK_EQ_I(R.t.want_hangup, 0);
    /* random drops: mean 1 minute, memoryless */
    c.drop_mean_min = 1;
    answer(&R, &c);
    for (k = 0; k < trials; k++) {
        R.hook = 0; run(&R, 400); R.hook = 1; run(&R, 100); dial(&R, c.number);
        wait_state(&R, TEL_ANSWERED, 5000);
        for (n = 0; R.t.state == TEL_ANSWERED && n < 1200L * FS; n++) tel_tick(&R.t, 1, 0);
        CHECK_EQ_I(R.t.state, TEL_ENDED);
        if (k == 0) CHECK_EQ_I(R.t.end, TEL_END_DROP);
        sum += (double)n / FS;
    }
    printf("  drop_mean 1 min: mean call %.1f s over %d calls\n", sum / trials, trials);
    CHECK(sum / trials > 48 && sum / trials < 72);
}

static void test_reconfigure(void)
{
    tel_cfg c = base_cfg(), c2;
    c.rings = 0;
    answer(&R, &c);
    c2 = c; strcpy(c2.number, "42");
    tel_reconfigure(&R.t, &c2);
    CHECK(strcmp(R.t.cfg.number, "5551234") == 0);   /* not during a call */
    R.hook = 0; run(&R, 600);
    CHECK(strcmp(R.t.cfg.number, "42") == 0);
    R.hook = 1; run(&R, 200); dial(&R, "42");
    CHECK(wait_state(&R, TEL_ANSWERED, 5000) >= 0);
    R.hook = 0; run(&R, 600);
    c2.busy_chance = 100;
    tel_reconfigure(&R.t, &c2);                 /* on hook: at once */
    CHECK_EQ_I(R.t.cfg.busy_chance, 100);
}

/* ---- the DTMF receiver on its own ---- */
static int dt_feed(tel_dtmf *d, double fl, double fh, double al, double ah, int ms, char *out, int *nout)
{
    int i, k;
    for (i = 0; i < MS(ms); i++) {
        double x = al * sin(2 * M_PI * fl * i / FS) + ah * sin(2 * M_PI * fh * i / FS + 0.7);
        k = tel_dtmf_rx(d, (int16_t)x);
        if (k && *nout < 60) out[(*nout)++] = (char)k;
    }
    return *nout;
}

static void test_dtmf_receiver(void)
{
    tel_dtmf d; char out[64]; int n = 0, i, k;
    uint32_t lcg = 99;
    /* every key, at three levels, both twists, detuned +-1.5 % */
    static const double lev[3] = { 8000, 1500, 250 };
    int lv, tw, dt, miss = 0, extra = 0;
    for (lv = 0; lv < 3; lv++) for (tw = 0; tw < 3; tw++) for (dt = 0; dt < 3; dt++) {
        double al = lev[lv] * (tw == 1 ? 0.5 : 1), ah = lev[lv] * (tw == 2 ? 0.5 : 1);
        double det = 1 + (dt - 1) * 0.015;
        tel_dtmf_init(&d); n = 0;
        for (i = 0; i < 16; i++) {
            dt_feed(&d, rowf[i / 4] * det, colf[i % 4] * det, al, ah, 50, out, &n);
            dt_feed(&d, 0, 0, 0, 0, 50, out, &n);
        }
        out[n] = 0;
        if (strcmp(out, keys) != 0) { miss++; printf("  level %g twist %d detune %d: got \"%s\"\n", lev[lv], tw, dt, out); }
    }
    CHECK_EQ_I(miss, 0);
    /* a long key press is one digit */
    tel_dtmf_init(&d); n = 0;
    dt_feed(&d, 770, 1336, 1500, 1500, 2000, out, &n);
    dt_feed(&d, 0, 0, 0, 0, 100, out, &n);
    CHECK_EQ_I(n, 1); CHECK_EQ_I(out[0], '5');
    /* a 20 ms blip is none */
    tel_dtmf_init(&d); n = 0;
    dt_feed(&d, 770, 1336, 1500, 1500, 20, out, &n);
    dt_feed(&d, 0, 0, 0, 0, 100, out, &n);
    CHECK_EQ_I(n, 0);
    /* nothing from: dial tone, single tones, a row pair, noise, an answer tone */
    tel_dtmf_init(&d); n = 0;
    dt_feed(&d, 350, 440, 3000, 3000, 2000, out, &n);
    dt_feed(&d, 697, 0, 3000, 0, 1000, out, &n);
    dt_feed(&d, 0, 1336, 0, 3000, 1000, out, &n);
    dt_feed(&d, 697, 770, 3000, 3000, 1000, out, &n);
    dt_feed(&d, 2100, 0, 5000, 0, 2000, out, &n);
    dt_feed(&d, 1200, 2400, 3000, 3000, 2000, out, &n);
    extra = n;
    for (i = 0; i < 80000; i++) {
        lcg = lcg * 1664525u + 1013904223u;
        k = tel_dtmf_rx(&d, (int16_t)((int32_t)(lcg >> 16) - 32768) / 4);
        if (k) extra++;
    }
    CHECK_EQ_I(extra, 0);
    /* a key through dial tone at the same level */
    tel_dtmf_init(&d); n = 0;
    for (i = 0; i < MS(70); i++) {
        double x = 1500 * sin(2 * M_PI * 852 * i / FS) + 1500 * sin(2 * M_PI * 1477 * i / FS)
                 + 1000 * sin(2 * M_PI * 350 * i / FS) + 1000 * sin(2 * M_PI * 440 * i / FS);
        k = tel_dtmf_rx(&d, (int16_t)x);
        if (k) out[n++] = (char)k;
    }
    CHECK_EQ_I(n, 1); CHECK_EQ_I(out[0], '9');
}

int main(void)
{
    test_more();
    test_dtmf_receiver();
    test_dialtone();
    test_dtmf_call();
    test_rings_zero_and_eu();
    test_wrong_number();
    test_trunk_one();
    test_also();
    test_any_number();
    test_pulse();
    test_busy();
    test_call_end();
    test_originate();
    test_limits();
    test_reconfigure();
    T_REPORT("tel");
}
