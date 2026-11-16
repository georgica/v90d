/* Detection phase tests: the ODP and ADP bit patterns of V.42 7.2.1 and
 * Table 3, their recognition rules, and start-stop character framing. */
#include <string.h>
#include "test.h"
#include "v42_detect.h"

static void pull(v42det_tx_t *t, char *out, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = (char)('0' + v42det_tx_bit(t));
    out[n] = 0;
}

static void test_patterns(void)
{
    v42det_tx_t t;
    char s[200];

    /* 7.2.1.2: 0 1000 1000 1 11...11 0 1000 1001 1 11...11 (8 to 16 ones) */
    v42det_tx_init(&t, V42DET_PAT_ODP, 8);
    CHECK(v42det_tx_boundary(&t));
    pull(&t, s, 36);
    CHECK(strcmp(s, "0100010001" "11111111" "0100010011" "11111111") == 0);
    CHECK(v42det_tx_boundary(&t));
    CHECK_EQ_I(t.patterns, 1);
    pull(&t, s, 36);                       /* and it repeats */
    CHECK(strcmp(s, "0100010001" "11111111" "0100010011" "11111111") == 0);
    CHECK_EQ_I(t.patterns, 2);

    /* Table 3: 0 1010 0010 1 11...11 0 1100 0010 1 11...11 */
    v42det_tx_init(&t, V42DET_PAT_ADP_V42, 8);
    pull(&t, s, 36);
    CHECK(strcmp(s, "0101000101" "11111111" "0110000101" "11111111") == 0);
    /* Table 3: 0 1010 0010 1 11...11 0 0000 0000 1 11...11 */
    v42det_tx_init(&t, V42DET_PAT_ADP_NOEC, 16);
    pull(&t, s, 52);
    CHECK(strcmp(s, "0101000101" "1111111111111111" "0000000001" "1111111111111111") == 0);
    pull(&t, s, 5);
    CHECK(!v42det_tx_boundary(&t));
    /* out of range separations are pulled into 8..16 */
    v42det_tx_init(&t, V42DET_PAT_ODP, 3);
    CHECK_EQ_I(t.gap, 8);
    v42det_tx_init(&t, V42DET_PAT_ODP, 99);
    CHECK_EQ_I(t.gap, 16);
}

/* feed one character with `gap` mark bits after its stop bit */
static int feed_char(v42det_rx_t *d, uint8_t c, int gap)
{
    int i, r = V42DET_NONE, x;
    x = v42det_rx_bit(d, 0); if (x) r = x;
    for (i = 0; i < 8; i++) { x = v42det_rx_bit(d, (c >> i) & 1); if (x) r = x; }
    x = v42det_rx_bit(d, 1); if (x) r = x;
    for (i = 0; i < gap; i++) { x = v42det_rx_bit(d, 1); if (x) r = x; }
    return r;
}

static void test_detect_odp(void)
{
    v42det_rx_t d; v42det_tx_t t;
    int i, gap, r;

    /* exactly four DC1s of alternating parity are needed (7.2.1.3) */
    v42det_rx_init(&d, 0);
    for (i = 0; i < 20; i++) v42det_rx_bit(&d, 1);
    CHECK_EQ_I(feed_char(&d, 0x11, 8), V42DET_NONE);
    CHECK_EQ_I(feed_char(&d, 0x91, 8), V42DET_NONE);
    CHECK_EQ_I(feed_char(&d, 0x11, 8), V42DET_NONE);
    CHECK_EQ_I(feed_char(&d, 0x91, 8), V42DET_ODP);
    CHECK_EQ_I(d.result, V42DET_ODP);

    /* starting on the odd parity DC1 is just as good */
    v42det_rx_init(&d, 0);
    feed_char(&d, 0x91, 12); feed_char(&d, 0x11, 12); feed_char(&d, 0x91, 12);
    CHECK_EQ_I(d.result, V42DET_NONE);
    feed_char(&d, 0x11, 12);
    CHECK_EQ_I(d.result, V42DET_ODP);

    /* every separation works for the ODP, the legal 8 to 16 and the others
     * that real modems use (test_detect_odp_real) */
    for (gap = 0; gap <= 24; gap++) {
        v42det_rx_init(&d, 0);
        for (i = 0; i < 6; i++) feed_char(&d, (i & 1) ? 0x91 : 0x11, gap);
        CHECK_EQ_I(d.result, V42DET_ODP);
    }

    /* same parity repeated is not the ODP; nor is ordinary text */
    v42det_rx_init(&d, 0);
    for (i = 0; i < 40; i++) feed_char(&d, 0x11, 8);
    CHECK_EQ_I(d.result, V42DET_NONE);
    v42det_rx_init(&d, 0);
    for (i = 0; i < 2000; i++) feed_char(&d, (uint8_t)("AT&F login: \r\n\x11\x13"[i % 16]), i % 20);
    CHECK_EQ_I(d.result, V42DET_NONE);

    /* a damaged character restarts the count */
    v42det_rx_init(&d, 0);
    feed_char(&d, 0x11, 8); feed_char(&d, 0x91, 8); feed_char(&d, 0x11, 8);
    feed_char(&d, 0x93, 8);
    feed_char(&d, 0x11, 8); feed_char(&d, 0x91, 8); feed_char(&d, 0x11, 8);
    CHECK_EQ_I(d.result, V42DET_NONE);
    CHECK_EQ_I(feed_char(&d, 0x91, 8), V42DET_ODP);

    /* our own sender is recognised by our own detector, and the ADP sender
     * is not mistaken for an ODP */
    v42det_rx_init(&d, 0);
    v42det_tx_init(&t, V42DET_PAT_ODP, 8);
    r = 0;
    for (i = 0; i < 36 * 2 && !r; i++) r = v42det_rx_bit(&d, v42det_tx_bit(&t));
    CHECK_EQ_I(r, V42DET_ODP);
    CHECK(i <= 36 + 18 + 10 + 1);          /* fourth DC1's stop bit */
    v42det_rx_init(&d, 0);
    v42det_tx_init(&t, V42DET_PAT_ADP_V42, 8);
    for (i = 0; i < 36 * 20; i++) v42det_rx_bit(&d, v42det_tx_bit(&t));
    CHECK_EQ_I(d.result, V42DET_NONE);
}

/* The ODP of a Conexant CX93001 in a V.34 call, as our receiver decoded it
 * on the box (2026-11-15): the marks between the characters are not the 8 to
 * 16 of 7.2.1.1 but 14, 22 or none at all. It is an ODP all the same. */
static void test_detect_odp_real(void)
{
    static const char real[] =
        "010001000111111111111111010001001111111111111111010001000111111111111111111111110100010011010001000111111111111101000100111111111111111111111111010001000111111111111111010001001111111111111111010001000111111111111111111111110100010011010001000111111111111101000100111111111111111111111111010001000111111111111111010001001111111111";
    v42det_rx_t d; int i, r = V42DET_NONE;
    v42det_rx_init(&d, 0);
    for (i = 0; i < 100; i++) (void)v42det_rx_bit(&d, 1);
    for (i = 0; real[i] && r == V42DET_NONE; i++) r = v42det_rx_bit(&d, real[i] - '0');
    CHECK_EQ_I(r, V42DET_ODP);
    CHECK(i <= 140);                        /* by the sixth character at the latest */
}

static void test_detect_adp(void)
{
    v42det_rx_t d; v42det_tx_t t;
    int i;

    /* two adjacent ADPs are required (7.2.1.2) */
    v42det_rx_init(&d, 1);
    feed_char(&d, 'E', 8); feed_char(&d, 'C', 8); feed_char(&d, 'E', 8);
    CHECK_EQ_I(d.result, V42DET_NONE);
    CHECK_EQ_I(feed_char(&d, 'C', 8), V42DET_ADP_V42);

    v42det_rx_init(&d, 1);
    feed_char(&d, 'E', 16); feed_char(&d, 0, 16); feed_char(&d, 'E', 16);
    CHECK_EQ_I(feed_char(&d, 0, 16), V42DET_ADP_NOEC);

    /* Appendix VI.1: "EM" five times, then "EC": only "EC" counts */
    v42det_rx_init(&d, 1);
    for (i = 0; i < 5; i++) { feed_char(&d, 'E', 8); feed_char(&d, 'M', 8); }
    CHECK_EQ_I(d.result, V42DET_NONE);
    feed_char(&d, 'E', 8); feed_char(&d, 'C', 8);
    CHECK_EQ_I(d.result, V42DET_NONE);
    feed_char(&d, 'E', 8); feed_char(&d, 'C', 8);
    CHECK_EQ_I(d.result, V42DET_ADP_V42);

    /* the ODP echoed back is not an ADP */
    v42det_rx_init(&d, 1);
    v42det_tx_init(&t, V42DET_PAT_ODP, 8);
    for (i = 0; i < 36 * 20; i++) v42det_rx_bit(&d, v42det_tx_bit(&t));
    CHECK_EQ_I(d.result, V42DET_NONE);

    /* sender to detector, preceded by answerer mark */
    v42det_rx_init(&d, 1);
    v42det_tx_init(&t, V42DET_PAT_ADP_V42, 10);
    for (i = 0; i < 100; i++) v42det_rx_bit(&d, 1);
    for (i = 0; i < 40 * 2; i++) v42det_rx_bit(&d, v42det_tx_bit(&t));
    CHECK_EQ_I(d.result, V42DET_ADP_V42);
}

static void test_async(void)
{
    v42async_tx_t t; v42async_rx_t r;
    int c, i, got, n;

    v42async_tx_init(&t); v42async_rx_init(&r);
    CHECK(v42async_tx_idle(&t));
    for (i = 0; i < 10; i++) CHECK(v42async_tx_bit(&t) == 1);     /* mark */
    /* all 256 values back to back, no idle time between characters */
    n = 0;
    for (c = 0; c < 256; c++) {
        CHECK(v42async_tx_idle(&t));
        v42async_tx_char(&t, (uint8_t)c);
        CHECK(!v42async_tx_idle(&t));
        for (i = 0; i < 10; i++) {
            got = v42async_rx_bit(&r, v42async_tx_bit(&t));
            if (got >= 0) { CHECK_EQ_I(got, c); CHECK_EQ_I(r.gap, 0); n++; }
        }
    }
    CHECK_EQ_I(n, 256);
    /* exact line bits of one character: start, lsb first, stop */
    v42async_tx_init(&t);
    v42async_tx_char(&t, 0x45);
    {
        char s[11];
        for (i = 0; i < 10; i++) s[i] = (char)('0' + v42async_tx_bit(&t));
        s[10] = 0;
        CHECK(strcmp(s, "0101000101") == 0);
    }
    /* gap is reported */
    v42async_rx_init(&r);
    for (i = 0; i < 5; i++) v42async_rx_bit(&r, 1);
    got = -1;
    for (i = 0; i < 10; i++) got = v42async_rx_bit(&r, "0101000101"[i] - '0');
    CHECK_EQ_I(got, 0x45);
    for (i = 0; i < 11; i++) v42async_rx_bit(&r, 1);
    for (i = 0; i < 10; i++) got = v42async_rx_bit(&r, "0110000101"[i] - '0');
    CHECK_EQ_I(got, 0x43);
    CHECK_EQ_I(r.gap, 11);

    /* a break (long space) gives one framing error, no stream of NULs, and
     * reception resumes after the line returns to mark */
    v42async_rx_init(&r);
    n = 0; c = 0;
    for (i = 0; i < 200; i++) {
        got = v42async_rx_bit(&r, 0);
        if (got == V42ASYNC_FRAMING) n++;
        if (got >= 0) c++;
    }
    CHECK_EQ_I(n, 1);
    CHECK_EQ_I(c, 0);
    for (i = 0; i < 3; i++) v42async_rx_bit(&r, 1);
    got = -1;
    for (i = 0; i < 10; i++) got = v42async_rx_bit(&r, "0101000101"[i] - '0');
    CHECK_EQ_I(got, 0x45);
}

int main(void)
{
    test_patterns();
    test_detect_odp();
    test_detect_odp_real();
    test_detect_adp();
    test_async();
    T_REPORT("test_detect");
}
