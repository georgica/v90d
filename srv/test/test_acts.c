/* The ACTS-format time service's core (srv/acts.h) on a simulated line:
 * octets leave at bps from the moment they are handed out, the host polls
 * every 10 ms, and a caller may echo the OTM after a round trip. */
#include "../acts.h"
#include "../../test/test.h"
#include <stdio.h>
#include <string.h>

#define US 1000000LL
/* 1990-04-18 21:39:15 UTC, NIST's own example line */
#define T_EXAMPLE 640474755LL

typedef struct {
    acts_t a;
    int64_t now, free_us;           /* the line is free from then */
    char rx[65536]; int64_t at[65536]; size_t n;    /* what the caller got, and when each octet arrived */
    int64_t rt_us;                  /* the caller echoes the OTM after this round trip; 0 = no echo */
    int64_t jit_us;                 /* every other echo this much later (a 10 ms loop's wander) */
    int64_t echo_at[64]; uint8_t echo_c[64]; int ne, ke;
} sim_t;

static void run(sim_t *s, int64_t until)
{
    while (s->now < until) {
        uint8_t b[256]; int64_t next; size_t n, i;
        while (s->ke < s->ne && s->echo_at[s->ke] <= s->now) {     /* echoes due */
            acts_rx(&s->a, &s->echo_c[s->ke], 1, s->now); s->ke++;
        }
        do {
            n = acts_poll(&s->a, s->now, b, sizeof b, &next);
            for (i = 0; i < n && s->n < sizeof s->rx; i++) {
                int64_t t = (s->free_us > s->now ? s->free_us : s->now) + s->a.oct_us;
                s->free_us = t;
                s->rx[s->n] = (char)b[i]; s->at[s->n] = t; s->n++;
                if ((b[i] == '*' || b[i] == '#') && s->rt_us && s->ne < 64) {  /* half there, half back */
                    s->echo_at[s->ne] = t + s->rt_us + (s->ne % 2 ? s->jit_us : 0); s->echo_c[s->ne] = b[i]; s->ne++;
                }
            }
        } while (n == sizeof b);
        if (acts_done(&s->a)) return;
        s->now += 10000;
    }
}

static void start(sim_t *s, int bps, int64_t t0, int64_t rt)
{
    acts_cfg c;
    memset(s, 0, sizeof *s);
    memset(&c, 0, sizeof c);
    c.bps = bps; c.dut1_tenths = 1;
    s->now = t0; s->rt_us = rt;
    acts_init(&s->a, &c, t0);
}

/* the OTMs: the second each names (the line before it), and when it arrived */
static int otms(const sim_t *s, int64_t *named, int64_t *arrived, char *mark, int max)
{
    size_t i; int k = 0;
    for (i = 0; i < s->n && k < max; i++) {
        if (s->rx[i] != '*' && s->rx[i] != '#') continue;
        if (i < 30 || memcmp(s->rx + i - 10, "UTC(NIST) ", 10) != 0) continue;
        {   /* HH:MM:SS of its line, the day from the simulation's start */
            const char *l = s->rx + i;
            while (l > s->rx && l[-1] != '\n') l--;
            int hh = (l[15] - '0') * 10 + (l[16] - '0'), mm = (l[18] - '0') * 10 + (l[19] - '0'), ss = (l[21] - '0') * 10 + (l[22] - '0');
            int64_t day = s->at[i] / US / 86400 * 86400;
            int64_t t = day + hh * 3600 + mm * 60 + ss;
            if (t < s->at[i] / US - 43200) t += 86400;
            named[k] = t; arrived[k] = s->at[i]; mark[k] = s->rx[i]; k++;
        }
    }
    return k;
}

int main(void)
{
    static sim_t s;
    char b[96];
    int64_t named[200], arr[200]; char mark[200];
    int k, i, bad;

    /* the fields */
    CHECK_EQ_I((int)acts_mjd(T_EXAMPLE), 47999);
    CHECK_EQ_I((int)acts_mjd(0), 40587);
    start(&s, 1200, T_EXAMPLE * US, 0);
    CHECK(acts_line(&s.a, T_EXAMPLE, b, sizeof b) > 0);
    CHECK(strcmp(b, "47999 90-04-18 21:39:15 50 0 +.1 045.0 UTC(NIST) ") == 0);
    if (strcmp(b, "47999 90-04-18 21:39:15 50 0 +.1 045.0 UTC(NIST) ") != 0) printf("    got \"%s\"\n", b);
    /* daylight time: 2026 changes on Sunday 8 March and Sunday 1 November */
    CHECK_EQ_I(acts_dst_code(1771200000LL), 0);           /* 2026-02-16 */
    CHECK_EQ_I(acts_dst_code(1772323200LL), 58);          /* 2026-03-01 00:00: 50 + 7 days + 1 */
    CHECK_EQ_I(acts_dst_code(1772841600LL), 52);          /* 2026-03-07 */
    CHECK_EQ_I(acts_dst_code(1772928000LL + 8 * 3600 + 3599), 51);   /* 2026-03-08 08:59:59 UTC */
    CHECK_EQ_I(acts_dst_code(1772928000LL + 9 * 3600), 50);          /* 09:00 UTC: 2 a.m. MST */
    CHECK_EQ_I(acts_dst_code(1790000000LL), 50);          /* 2026-09-21 */
    CHECK_EQ_I(acts_dst_code(1793491200LL + 7 * 3600 + 3599), 1);    /* 2026-11-01 07:59:59 UTC */
    CHECK_EQ_I(acts_dst_code(1793491200LL + 8 * 3600), 0);           /* 08:00 UTC: 2 a.m. MDT */
    CHECK_EQ_I(acts_dst_code(1793404800LL), 50);          /* 2026-10-31 */
    CHECK_EQ_I(acts_dst_code(1762041600LL - 86400), 2);   /* 2025-11-01: change on the 2nd */
    CHECK_EQ_I(acts_dst_code(1798761600LL), 0);           /* 2027-01-01 */

    /* a call at 1200 bit/s, nobody echoing: the banner, then a line a second,
     * each OTM arriving 45 ms before its second, within the 10 ms of the loop */
    start(&s, 1200, 1795000000LL * US + 123456, 0);
    run(&s, s.now + 20 * US);
    CHECK(s.n > 0 && memcmp(s.rx, "\r\nNational Institute of Standards and Technology\r\n"
                                  "Telephone Time Service, Generator 3B\r\n"
                                  "Enter question mark \"?\" for HELP\r\n", 120) == 0);
    CHECK(strstr(s.rx, " MJD  YR MO DA H  M  S  ST S UT1 msADV         OTM\r\n") != NULL);
    k = otms(&s, named, arr, mark, 200);
    printf("  1200 bit/s: %d lines in 20 s\n", k);
    CHECK(k >= 18);
    for (i = bad = 0; i < k; i++) {
        /* handed out msADV before its second, give or take half the loop;
         * here it has arrived one octet later */
        int64_t early = named[i] * US - arr[i] + s.a.oct_us;
        if (early < 45000 - 5000 || early > 45000 + 5000) bad++;
        if (i && named[i] != named[i - 1] + 1) bad++;
        if (mark[i] != '*') bad++;
    }
    CHECK_EQ_I(bad, 0);
    if (bad) for (i = 0; i < k; i++) printf("    %lld early %lld us %c\n", (long long)named[i], (long long)(named[i] * US - arr[i]), mark[i]);
    CHECK(strstr(s.rx, " 045.0 UTC(NIST) *\r\n") != NULL);

    /* echoing after a 120 ms round trip: msADV 60 ms, then # after three */
    start(&s, 9600, 1795000000LL * US, 120000);
    run(&s, s.now + 12 * US);
    k = otms(&s, named, arr, mark, 200);
    CHECK(k >= 10);
    for (i = 0; i < k && mark[i] == '*'; i++) ;
    printf("  echoed (120 ms round trip): the first # on line %d; msADV now %.1f ms\n", i + 1, s.a.adv_us / 1000.0);
    CHECK(i >= 3 && i <= 5);
    for (bad = 0; i < k; i++) if (mark[i] != '#') bad++;
    CHECK_EQ_I(bad, 0);
    CHECK(s.a.adv_us >= 60000 - 6000 && s.a.adv_us <= 60000 + 6000);
    {   /* once measured, the OTM arrives about msADV before the second... and
         * the echo half (60 ms) is what the caller loses: arrival + 60 ms, i.e.
         * the caller's clock is right to within the loop's 10 ms */
        int64_t early = named[k - 1] * US - arr[k - 1] + s.a.oct_us;
        printf("    last OTM handed out %.1f ms before its second (msADV %.1f)\n", early / 1000.0, s.a.adv_us / 1000.0);
        CHECK(early >= s.a.adv_us - 5000 && early <= s.a.adv_us + 5000);
        CHECK(strstr(s.rx, "UTC(NIST) #\r\n") != NULL);
    }
    /* one echo that does not agree is let go; two in a row start again */
    {
        int was = s.a.adv_us, ag = s.a.agree;
        s.rt_us = 300000;
        run(&s, s.now + 1 * US);
        printf("    one odd echo: agree %d -> %d, msADV %.1f -> %.1f\n", ag, s.a.agree, was / 1000.0, s.a.adv_us / 1000.0);
        CHECK_EQ_I(s.a.adv_us, was); CHECK(s.a.agree >= ACTS_AGREE_N);
        run(&s, s.now + 2 * US);
        CHECK(s.a.agree < ACTS_AGREE_N);
        CHECK(s.a.adv_us > 140000);
    }

    /* echoes that wander by a 10 ms block (the box's loop): still #, and
     * msADV steadies near the middle of the two */
    start(&s, 9600, 1795000000LL * US, 120000);
    s.jit_us = 10000;
    run(&s, s.now + 15 * US);
    k = otms(&s, named, arr, mark, 200);
    for (i = 0; i < k && mark[i] == '*'; i++) ;
    for (bad = 0; i < k; i++) if (mark[i] != '#') bad++;
    printf("  echoes 10 ms apart in turn: %d lines, %d not # after the first; msADV %.1f ms\n", k, bad, s.a.adv_us / 1000.0);
    CHECK_EQ_I(bad, 0);
    CHECK(s.a.adv_us >= 60000 && s.a.adv_us <= 70000);

    /* 300 bit/s: a line each two seconds, on even seconds */
    start(&s, 300, 1795000000LL * US, 0);
    run(&s, s.now + 20 * US);
    k = otms(&s, named, arr, mark, 200);
    printf("  300 bit/s: %d lines in 20 s\n", k);
    CHECK(k >= 5 && k <= 8);         /* the banner takes 6 s at 300 bit/s */
    for (i = bad = 0; i < k; i++) {
        int64_t early = named[i] * US - arr[i] + s.a.oct_us;
        if (named[i] % 2) bad++;
        if (i && named[i] != named[i - 1] + 2) bad++;
        if (early < 45000 - 5000 || early > 45000 + 5000) bad++;
    }
    CHECK_EQ_I(bad, 0);

    /* '?': the help and the heads, then the lines go on */
    start(&s, 2400, 1795000000LL * US, 0);
    run(&s, s.now + 3 * US);
    acts_rx(&s.a, (const uint8_t *)"?", 1, s.now);
    run(&s, s.now + 8 * US);
    CHECK(strstr(s.rx, "Send the OTM back") != NULL);
    k = otms(&s, named, arr, mark, 200);
    CHECK(k >= 6);
    for (i = 1, bad = 0; i < k; i++) if (named[i] <= named[i - 1]) bad++;
    CHECK_EQ_I(bad, 0);

    /* the end: after max_s the line in hand ends, then done */
    {
        acts_cfg c; memset(&c, 0, sizeof c); c.bps = 1200; c.max_s = 10;
        memset(&s, 0, sizeof s); s.now = 1795000000LL * US;
        acts_init(&s.a, &c, s.now);
        run(&s, s.now + 30 * US);
        CHECK(acts_done(&s.a));
        printf("  max_s 10: done after %.2f s, %zu octets\n", (s.now - 1795000000LL * US) / 1e6, s.n);
        CHECK(s.now - 1795000000LL * US >= 10 * US && s.now - 1795000000LL * US <= 12 * US);
        CHECK(s.n >= 2 && s.rx[s.n - 2] == '\r' && s.rx[s.n - 1] == '\n');
        CHECK(s.free_us <= s.now);          /* all of it out before the hang-up */
    }

    T_REPORT("acts");
}
