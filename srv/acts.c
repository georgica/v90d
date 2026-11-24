/* The ACTS-format time service's core (acts.h). Ours; written for this project. */
#include "acts.h"
#include <stdio.h>
#include <string.h>

#define US 1000000LL

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

/* days since 1970-01-01 to the civil date (Howard Hinnant's algorithm) */
static void civil(int64_t z, int *y, int *m, int *d)
{
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = floor_div(z, 146097);
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* the day of the month of the n-th Sunday (n >= 1) of y-m */
static int nth_sunday(int y, int m, int n)
{
    /* days since 1970-01-01 of y-m-01 (the inverse of civil) */
    int64_t yy = y - (m <= 2), era = floor_div(yy, 400), yoe = yy - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int wd = (int)((days % 7 + 7 + 4) % 7);      /* 1970-01-01 was a Thursday; 0 = Sunday */
    return 1 + (7 - wd) % 7 + 7 * (n - 1);
}

long acts_mjd(int64_t unix_s) { return (long)(floor_div(unix_s, 86400) + 40587); }

int acts_dst_code(int64_t unix_s)
{
    int y, m, d, chg;
    int64_t day = floor_div(unix_s, 86400);
    int hour = (int)((unix_s - day * 86400) / 3600);
    civil(day, &y, &m, &d);
    if (m < 3 || m > 11) return 0;
    if (m > 3 && m < 11) return 50;
    if (m == 3) {               /* 2 a.m. MST is 09:00 UTC */
        chg = nth_sunday(y, 3, 2);
        if (d < chg) return 50 + chg - d + 1;
        if (d == chg) return hour < 9 ? 51 : 50;
        return 50;
    }
    chg = nth_sunday(y, 11, 1);  /* 2 a.m. MDT is 08:00 UTC */
    if (d < chg) return chg - d + 1;
    if (d == chg) return hour < 8 ? 1 : 0;
    return 0;
}

int acts_line(const acts_t *a, int64_t unix_s, char *b, size_t cap)
{
    int y, m, d, dut = a->c.dut1_tenths, adv = (a->adv_us + 50) / 100;  /* 0.1 ms */
    int64_t day = floor_div(unix_s, 86400), sod = unix_s - day * 86400;
    int n;
    civil(day, &y, &m, &d);
    if (dut > 9) dut = 9;
    if (dut < -9) dut = -9;
    if (adv > 9999) adv = 9999;
    if (adv < 0) adv = 0;
    n = snprintf(b, cap, "%05ld %02d-%02d-%02d %02d:%02d:%02d %02d %d %c.%d %03d.%d UTC(NIST) ",
                 acts_mjd(unix_s) % 100000, y % 100, m, d, (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60),
                 acts_dst_code(unix_s), a->c.leap >= 0 && a->c.leap <= 2 ? a->c.leap : 0,
                 dut < 0 ? '-' : '+', dut < 0 ? -dut : dut, adv / 10, adv % 10);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

static const char HEADS[] =
    "                         D  L D\r\n"
    " MJD  YR MO DA H  M  S  ST S UT1 msADV         OTM\r\n";

static const char HELP[] =
    "\r\n"
    "A line of the time follows each second (each two seconds below 1200\r\n"
    "bit/s): the Modified Julian Date, the date and time in UTC, the US\r\n"
    "daylight time code (00 standard, 50 daylight, counting down in the month\r\n"
    "of a change), a leap second flag, UT1 - UTC in tenths of a second, the\r\n"
    "advance in milliseconds, and the on-time marker (OTM). The time named is\r\n"
    "the moment the OTM reaches you. Send the OTM back as it comes and the\r\n"
    "advance is measured for your line: half the round trip. Once three\r\n"
    "measurements agree the OTM is # instead of *.\r\n"
    "\r\n";

static void put(acts_t *a, const void *p, size_t n, int64_t now)
{
    int64_t from = a->busy_us > now ? a->busy_us : now;
    if (n > sizeof a->q - a->qn) n = sizeof a->q - a->qn;
    memcpy(a->q + a->qn, p, n); a->qn += n;
    a->busy_us = from + (int64_t)n * a->oct_us;
}

/* the next line: the first second it can name with its text out in time */
static void plan(acts_t *a, int64_t now)
{
    int64_t from = a->busy_us > now ? a->busy_us : now, s;
    s = floor_div(from, US) + 1;
    if (a->period > 1 && s % a->period) s += a->period - s % a->period;
    for (;; s += a->period) {
        a->text_n = acts_line(a, s, a->text, sizeof a->text);
        if (a->text_n < 0) a->text_n = 0;
        a->otm_us = s * US - a->adv_us;
        a->text_us = a->otm_us - (int64_t)a->text_n * a->oct_us - ACTS_MARGIN_US;
        if (a->text_us >= from) break;
    }
    a->sec = s; a->stage = 0;
}

void acts_init(acts_t *a, const acts_cfg *c, int64_t utc_us)
{
    char b[160];
    int n;
    memset(a, 0, sizeof *a);
    a->c = *c;
    if (a->c.bps < 300) a->c.bps = 300;
    if (a->c.max_s <= 0) a->c.max_s = ACTS_MAX_S;
    a->oct_us = 10 * US / a->c.bps;
    a->period = a->c.bps < 1200 ? 2 : 1;
    a->adv_us = ACTS_ADV0_US;
    a->start_us = utc_us;
    n = snprintf(b, sizeof b, "\r\nNational Institute of Standards and Technology\r\n"
                 "Telephone Time Service, Generator %s\r\n"
                 "Enter question mark \"?\" for HELP\r\n",
                 a->c.generator ? a->c.generator : "3B");
    if (n > 0) put(a, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1, utc_us);
    put(a, HEADS, sizeof HEADS - 1, utc_us);
    plan(a, utc_us);
}

void acts_rx(acts_t *a, const uint8_t *p, size_t n, int64_t utc_us)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (p[i] == '?') a->help = 1;
        else if ((p[i] == '*' || p[i] == '#') && a->sent_us && !a->echoed && utc_us >= a->sent_us
                 && utc_us - a->sent_us < US) {
            int adv = (int)((utc_us - a->sent_us) / 2), diff;
            a->echoed = 1;
            if (adv > 999900) adv = 999900;
            diff = adv - a->adv_us;
            if (a->agree > 0 && (diff > ACTS_AGREE_US || diff < -ACTS_AGREE_US)) {
                if (++a->odd < 2) continue;         /* one is let go */
                a->agree = 0;
            }
            a->odd = 0;
            /* the first of a run is taken as it is; the next ones go into
             * the run's mean (up to the last eight), which the loop's
             * jitter then moves less */
            a->adv_us = a->agree ? a->adv_us + diff / (a->agree < 8 ? a->agree + 1 : 8) : adv;
            a->agree++;
        }
    }
}

size_t acts_poll(acts_t *a, int64_t now, uint8_t *out, size_t cap, int64_t *next_us)
{
    size_t n;
    if (!a->done && !a->qn) {
        if (a->ending) {
            if (now >= a->busy_us) a->done = 1;
        } else if (a->stage == 0 && a->help) {
            a->help = 0;
            put(a, HELP, sizeof HELP - 1, now);
            put(a, HEADS, sizeof HEADS - 1, now);
            plan(a, now);
        } else if (a->stage == 0 && now >= a->text_us) {
            a->text_n = acts_line(a, a->sec, a->text, sizeof a->text);    /* msADV as it is now */
            if (a->text_n < 0) a->text_n = 0;
            a->otm_us = a->sec * US - a->adv_us;
            put(a, a->text, (size_t)a->text_n, now);
            a->stage = 1;
        } else if (a->stage == 1 && now >= a->otm_us - ACTS_EARLY_US) {
            uint8_t otm = a->agree >= ACTS_AGREE_N ? '#' : '*';
            put(a, &otm, 1, now);
            put(a, "\r\n", 2, now);
            a->sent_us = now; a->echoed = 0;
            if (now - a->start_us >= (int64_t)a->c.max_s * US) a->ending = 1;
            else plan(a, now);
        }
    }
    n = a->qn < cap ? a->qn : cap;
    memcpy(out, a->q, n);
    memmove(a->q, a->q + n, a->qn - n); a->qn -= n;
    if (next_us) {
        if (a->qn || a->done) *next_us = now;
        else if (a->ending) *next_us = a->busy_us;
        else if (a->help && a->stage == 0) *next_us = now;
        else *next_us = a->stage == 0 ? a->text_us : a->otm_us - ACTS_EARLY_US;
    }
    return n;
}

int acts_done(const acts_t *a) { return a->done; }
