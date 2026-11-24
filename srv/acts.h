/* A dial-in time service in the format of NIST's Automated Computer Time
 * Service (ACTS), so that period time-setting programs and the NTP ACTS
 * driver work against it unchanged. The pure core: no I/O and no clock of
 * its own; the host gives it the UTC time (utc_us, microseconds since the
 * Unix epoch, from an NTP-set clock) and writes what acts_poll() returns to
 * the caller at once. Shared with Netstalgia's nsdiald, which compiles this
 * file from v90d. Ours; written for this project.
 *
 * The call: the banner, then a line each second (each 2 s below 1200 bit/s):
 *
 *   MJD   YR-MO-DA HH:MM:SS TT L DUT1 msADV UTC(NIST) OTM
 *   61003 26-11-24 21:39:15 00 0 +.1 045.0 UTC(NIST) *
 *
 * - TT: the US daylight time code. 00 standard time, 50 daylight time. In
 *   the month of a change it counts the days down to it: in March from
 *   50 + the days to the change down to 51 on the day itself, 50 from 2 a.m.
 *   (Mountain time, NIST's own); in November from the days to the change
 *   down to 01, 00 from 2 a.m. The changes are the US rule since 2007:
 *   the second Sunday in March, the first Sunday in November.
 * - L: 0, or 1 / 2 when a leap second is added / deleted at the end of the
 *   month (the host says which).
 * - DUT1 in tenths of a second (UT1 - UTC), as the host says.
 * - msADV: how far ahead of the named second the OTM leaves, in ms. The
 *   time named is the moment the OTM reaches the caller: the OTM is handed
 *   out at that second less msADV. It starts at 45.0 ms; a caller that
 *   echoes the OTM back has it measured, half the round trip (the mean
 *   of the measurements that agree, ACTS_AGREE_US), and after three that
 *   agree the OTM changes from * to #. One echo that does not agree is let
 *   go (a caller's own hiccup); two in a row start the measurement again.
 *   The line before the OTM leaves early enough to be out at bps (10 bits
 *   an octet) by then; the CR LF follows the OTM.
 * - '?' from the caller: a short help text and the column heads, then the
 *   lines go on.
 * - After max_s seconds (default 55) the current line ends and acts_done()
 *   says hang up, once what was handed out is out at bps.
 *
 * The host calls acts_poll() at least every 10 ms (or at *next_us). The
 * OTM goes at the first call from ACTS_EARLY_US before its time: with a
 * 10 ms loop within 5 ms of it either way. The echo is timed from when it
 * went, so the loop does not upset the measurement beyond that.
 *
 * It says NIST and UTC(NIST) because clients look for that format; the
 * owner chose that, and the number NIST's service had (srv.h, SRV_ACTS). */
#ifndef V90D_ACTS_H
#define V90D_ACTS_H
#include <stddef.h>
#include <stdint.h>

#define ACTS_ADV0_US    45000   /* msADV before any echo */
#define ACTS_AGREE_US   6000    /* echoes within this of msADV agree: more than the half block (5 ms) a 10 ms loop adds */
#define ACTS_AGREE_N    3       /* this many in a row: the OTM is # */
#define ACTS_MAX_S      55      /* the call, by default */
#define ACTS_MARGIN_US  30000   /* the line is out this long before its OTM is due */
#define ACTS_EARLY_US   5000    /* the OTM goes at the first poll from this long before its time: half a 10 ms loop */

typedef struct {
    int bps;            /* the line's rate towards the caller, bit/s (>= 300) */
    int dut1_tenths;    /* UT1 - UTC in 0.1 s, -9..9 */
    int leap;           /* 0, 1 = a leap second added at the end of this month, 2 = deleted */
    int max_s;          /* how long the call lasts; 0 = ACTS_MAX_S */
    const char *generator;  /* the banner's "Generator 3B"; NULL = "3B" */
} acts_cfg;

typedef struct {
    acts_cfg c;
    int64_t  start_us;      /* acts_init */
    int64_t  oct_us;        /* one octet at bps */
    int64_t  busy_us;       /* what was handed out is out at bps by then */
    int      period;        /* seconds between lines: 1, or 2 below 1200 bit/s */
    int64_t  sec;           /* the second the line being sent names; 0 = none yet */
    int      stage;         /* 0 its text is due, 1 its OTM is due */
    int64_t  text_us, otm_us;   /* when they are due */
    int64_t  sent_us;       /* when the last OTM was handed out; 0 = none */
    int      echoed;        /* that OTM's echo came */
    int      adv_us;        /* msADV now */
    int      agree;         /* echoes in a row that agree */
    int      odd;           /* echoes in a row that did not (one is let go) */
    int      help;          /* '?' came: help before the next line */
    int      ending, done;
    char     text[80]; int text_n;
    uint8_t  q[1024];       /* due now: handed out by acts_poll */
    size_t   qn;
} acts_t;

/* a call is answered: the banner is queued */
void   acts_init(acts_t *a, const acts_cfg *c, int64_t utc_us);
/* octets from the caller, as they came at utc_us: '?' and OTM echoes */
void   acts_rx(acts_t *a, const uint8_t *p, size_t n, int64_t utc_us);
/* the octets to write to the caller now (at most cap; call again while it
 * fills cap); *next_us: when to call again at the latest */
size_t acts_poll(acts_t *a, int64_t utc_us, uint8_t *out, size_t cap, int64_t *next_us);
/* 1: hang up (max_s is over and its last line is out) */
int    acts_done(const acts_t *a);

/* for the tests and the host's status: the fields of the line for second s */
int    acts_dst_code(int64_t unix_s);
long   acts_mjd(int64_t unix_s);
int    acts_line(const acts_t *a, int64_t unix_s, char *b, size_t cap);   /* without the OTM */

#endif
