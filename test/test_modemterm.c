/* The rig core against the calling-modem peer, in 20 ms mu-law frames. */
#include <string.h>
#include "test.h"
#include "../rig/modemterm.h"
#include "callpeer.h"
#include "g711.h"

static char logbuf[4096];
static void logger(const char *l) { if (strlen(logbuf) + strlen(l) + 2 < sizeof logbuf) { strcat(logbuf, l); strcat(logbuf, "\n"); } }

int main(void)
{
    static mterm_t m; static callpeer_t c;
    uint8_t in[160], out[160];
    char got[256]; int ng = 0, f, i, sent = 0;

    memset(out, 0xFF, sizeof out);
    mterm_init(&m, 3000, V90M_MOD_V22 | V90M_MOD_V21, logger);
    callpeer_init(&c, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
    for (f = 0; f < 50 * 12; f++) {                    /* 12 s */
        /* the caller hears our previous frame (20 ms round-trip skew, as with RTP) */
        for (i = 0; i < 160; i++)
            in[i] = linear_to_ulaw(callpeer_tick(&c, (int16_t)ulaw_to_linear(out[i])));
        mterm_frame(&m, in, out, 160);
        if (c.connected && !sent && f > 300) { v14_write(&c.dte, (const uint8_t *)"ping 123\r", 9); sent = 1; }
        ng += v14_read(&c.dte, (uint8_t *)got + ng, 255 - ng);
    }
    got[ng] = 0;
    CHECK(c.connected);
    CHECK(strstr(got, "v90d CONNECT 2400") != NULL);   /* banner */
    CHECK(strstr(got, "ping 123\r") != NULL);          /* echo */
    CHECK(strstr(logbuf, "CONNECT") != NULL);
    CHECK(strstr(logbuf, "rx: ping 123") != NULL);
    CHECK_EQ_I(m.rx_chars, 9);

    T_REPORT("modemterm");
}
