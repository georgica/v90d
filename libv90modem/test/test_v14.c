/* Tests for the start-stop converter. */
#include <string.h>
#include "test.h"
#include "../modem/v14.h"

int main(void)
{
    static v14_t a, b;
    uint8_t buf[600], in[600];
    int i, n, bad;

    /* idle line is mark */
    v14_init(&a);
    bad = 0; for (i = 0; i < 50; i++) if (v14_getbit(&a) != 1) bad++;
    CHECK_EQ_I(bad, 0);

    /* one character on the wire: start, LSB first, stop */
    v14_write(&a, (const uint8_t *)"\xC1", 1);
    {
        static const int want[10] = {0, 1,0,0,0,0,0,1,1, 1};
        bad = 0; for (i = 0; i < 10; i++) if (v14_getbit(&a) != want[i]) bad++;
        CHECK_EQ_I(bad, 0);
        CHECK_EQ_I(v14_getbit(&a), 1);                /* back to idle */
    }

    /* round trip of every byte value, back to back, with idle gaps of any length */
    v14_init(&a); v14_init(&b);
    for (i = 0; i < 256; i++) in[i] = (uint8_t)i;
    CHECK_EQ_I(v14_write(&a, in, 256), 256);
    CHECK_EQ_I(v14_tx_pending(&a), 256);
    for (i = 0; i < 2560 + 7; i++) v14_putbit(&b, v14_getbit(&a));
    CHECK_EQ_I(v14_tx_pending(&a), 0);
    n = v14_read(&b, buf, 600);
    CHECK_EQ_I(n, 256);
    CHECK(!memcmp(buf, in, 256));
    v14_write(&a, in + 10, 3);
    for (i = 0; i < 37; i++) v14_putbit(&b, v14_getbit(&a));
    v14_write(&a, in + 99, 2);
    for (i = 0; i < 40; i++) v14_putbit(&b, v14_getbit(&a));
    n = v14_read(&b, buf, 600);
    CHECK_EQ_I(n, 5);
    CHECK(buf[0] == 10 && buf[2] == 12 && buf[3] == 99 && buf[4] == 100);

    /* the queue refuses what does not fit, and never corrupts */
    v14_init(&a);
    memset(in, 0x55, 600);
    n = v14_write(&a, in, 600);
    CHECK(n < 600 && n >= V14_QSIZE - 1);

    /* V.14 stop-bit deletion: "A" "B" with the stop of A missing still decodes */
    v14_init(&b);
    {
        static const int bits[] = {1,1, 0, 1,0,0,0,0,0,1,0, /* stop deleted */ 0, 0,1,0,0,0,0,1,0, 1, 1,1};
        for (i = 0; i < (int)(sizeof bits / sizeof bits[0]); i++) v14_putbit(&b, bits[i]);
        n = v14_read(&b, buf, 600);
        CHECK_EQ_I(n, 2);
        CHECK(buf[0] == 'A' && buf[1] == 'B');
    }
    /* a long space (break) is not a flood of NULs */
    v14_init(&b);
    for (i = 0; i < 200; i++) v14_putbit(&b, 0);
    for (i = 0; i < 20; i++) v14_putbit(&b, 1);
    n = v14_read(&b, buf, 600);
    CHECK(n <= 1);
    CHECK(b.rx_frame_errs >= 1);

    T_REPORT("v14");
}
