/* Tests for the async receiver and the V.8 sequence assembler. */
#include <string.h>
#include "test.h"
#include "../modem/async.h"
#include "../modem/v8rx.h"

#define FS 8000
static uint8_t lvl[60000];

/* render bits to a per-sample level stream at `spb` samples per bit */
static int render(const uint8_t *bits, int nb, double spb, double t0, uint8_t *out, int max)
{
    int n = (int)(t0 + nb * spb), i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        double t = (i - t0) / spb;
        out[i] = (t < 0) ? 1 : bits[(int)t];
    }
    return n;
}

static int put_char(uint8_t c, uint8_t *bits, int k)
{
    int i; bits[k++] = 0;
    for (i = 0; i < 8; i++) bits[k++] = (c >> i) & 1;
    bits[k++] = 1; return k;
}

int main(void)
{
    static uint8_t bits[2000];
    uint8_t got[64];
    arx_t a;
    int nb, n, i, ng, r;
    double rates[3] = {8000.0 / 300.0, 8000.0 / 303.0, 8000.0 / 297.0};
    int ri;

    /* characters back to back, at nominal and +/-1% baud, any start phase */
    for (ri = 0; ri < 3; ri++) {
        nb = 0;
        for (i = 0; i < 12; i++) bits[nb++] = 1;
        for (i = 0; i < 20; i++) nb = put_char((uint8_t)(i * 37 + 5), bits, nb);
        for (i = 0; i < 4; i++) bits[nb++] = 1;
        n = render(bits, nb, rates[ri], 3.3 + ri * 7.1, lvl, 60000);
        arx_init(&a, FS, 300);
        ng = 0;
        for (i = 0; i < n; i++) { r = arx_tick(&a, lvl[i]); if (r >= 0 && ng < 64) got[ng++] = (uint8_t)r; }
        CHECK_EQ_I(ng, 20);
        { int bad = 0; for (i = 0; i < 20 && i < ng; i++) if (got[i] != (uint8_t)(i * 37 + 5)) bad++; CHECK_EQ_I(bad, 0); }
    }

    /* idle measurement: 12 ones before the first char, 0 between chars */
    nb = 0;
    for (i = 0; i < 12; i++) bits[nb++] = 1;
    nb = put_char(0xE0, bits, nb); nb = put_char(0xC1, bits, nb);
    for (i = 0; i < 15; i++) bits[nb++] = 1;
    n = render(bits, nb, rates[0], 0, lvl, 60000);
    arx_init(&a, FS, 300);
    ng = 0;
    for (i = 0; i < n; i++) {
        r = arx_tick(&a, lvl[i]);
        if (r >= 0) { if (ng == 0) CHECK(arx_pre_bits(&a) >= 10); else CHECK(arx_pre_bits(&a) <= 1); ng++; }
    }
    CHECK_EQ_I(ng, 2);
    CHECK(arx_idle_bits(&a) >= 13);

    /* a glitch shorter than half a bit is not a start bit; bad stop = frame error */
    arx_init(&a, FS, 300);
    ng = 0;
    for (i = 0; i < 400; i++) { r = arx_tick(&a, (i >= 100 && i < 108) ? 0 : 1); if (r != ARX_NONE) ng++; }
    CHECK_EQ_I(ng, 0);
    arx_init(&a, FS, 300);
    ng = 0;
    for (i = 0; i < 600; i++) { r = arx_tick(&a, (i >= 100 && i < 400) ? 0 : 1); if (r == ARX_FRAMEERR) ng++; }
    CHECK_EQ_I(ng, 1);

    /* ---- V.8 sequence assembler ---- */
    {
        v8rx_t rx;
        uint8_t oct[5] = {0xC1, 0x45, 0x13, 0x90, 0x2A};
        int ev, first_menu = -1, cj_at = -1, rep, end2;

        nb = 0; end2 = 0;
        for (rep = 0; rep < 3; rep++) {
            nb += v8_frame(V8_SYNC_CM_JM, oct, 5, bits + nb, 2000 - nb);
            if (rep == 1) end2 = nb;
        }
        /* CJ cuts in after the first octet of a 4th sequence */
        nb += v8_frame(V8_SYNC_CM_JM, oct, 1, bits + nb, 2000 - nb);
        for (i = 0; i < 3; i++) nb = put_char(0x00, bits, nb);
        for (i = 0; i < 20; i++) bits[nb++] = 1;
        n = render(bits, nb, rates[0], 5.5, lvl, 60000);
        v8rx_init(&rx, FS);
        for (i = 0; i < n; i++) {
            ev = v8rx_tick(&rx, lvl[i]);
            if ((ev & V8RX_MENU) && first_menu < 0) {
                first_menu = i;   /* msg is valid at the event */
                CHECK_EQ_I(rx.nmsg, 5);
                CHECK(!memcmp(rx.msg, oct, 5));
            }
            if ((ev & V8RX_CJ) && cj_at < 0) cj_at = i;
        }
        /* menu reported once the 2nd identical sequence is complete: not before
         * its end, and within ~10 bit times after (the next preamble) */
        CHECK(first_menu >= (int)(end2 * rates[0]) - 30);
        CHECK(first_menu <= (int)((end2 + 11) * rates[0]));
        CHECK(cj_at > first_menu);

        /* two DIFFERENT sequences: no menu event */
        {
            uint8_t o2[5] = {0xC1, 0x45, 0x13, 0x10, 0x2A};
            int any = 0;
            nb = v8_frame(V8_SYNC_CM_JM, oct, 5, bits, 2000);
            nb += v8_frame(V8_SYNC_CM_JM, o2, 5, bits + nb, 2000 - nb);
            for (i = 0; i < 20; i++) bits[nb++] = 1;
            n = render(bits, nb, rates[0], 0, lvl, 60000);
            v8rx_init(&rx, FS);
            for (i = 0; i < n; i++) any |= v8rx_tick(&rx, lvl[i]);
            CHECK_EQ_I(any, 0);
        }
        /* a CI sequence (sync 0x00) is not a menu and one zero octet is not CJ */
        {
            int any = 0, rep2;
            nb = 0;
            for (rep2 = 0; rep2 < 4; rep2++) nb += v8_frame(V8_SYNC_CI, oct, 1, bits + nb, 2000 - nb);
            for (i = 0; i < 20; i++) bits[nb++] = 1;
            n = render(bits, nb, rates[0], 0, lvl, 60000);
            v8rx_init(&rx, FS);
            for (i = 0; i < n; i++) any |= v8rx_tick(&rx, lvl[i]);
            CHECK_EQ_I(any, 0);
        }
    }

    T_REPORT("v8rx");
}
