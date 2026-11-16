/* MD5 unit test: the RFC 1321 appendix A.5 test suite, plus split updates. */
#include <string.h>
#include "test.h"
#include "ppp_md5.h"

static void hex(const uint8_t d[16], char out[33])
{
    static const char *x = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i] = x[d[i] >> 4];
        out[2 * i + 1] = x[d[i] & 15];
    }
    out[32] = 0;
}

static void one(const char *msg, const char *want)
{
    ppp_md5_t m;
    uint8_t d[16];
    char h[33];
    size_t n = strlen(msg);

    ppp_md5_init(&m);
    ppp_md5_update(&m, (const uint8_t *)msg, n);
    ppp_md5_final(&m, d);
    hex(d, h);
    CHECK(strcmp(h, want) == 0);

    /* same message fed one byte at a time */
    ppp_md5_init(&m);
    for (size_t i = 0; i < n; i++)
        ppp_md5_update(&m, (const uint8_t *)msg + i, 1);
    ppp_md5_final(&m, d);
    hex(d, h);
    CHECK(strcmp(h, want) == 0);

    /* same message in two uneven pieces */
    ppp_md5_init(&m);
    ppp_md5_update(&m, (const uint8_t *)msg, n / 3);
    ppp_md5_update(&m, (const uint8_t *)msg + n / 3, n - n / 3);
    ppp_md5_final(&m, d);
    hex(d, h);
    CHECK(strcmp(h, want) == 0);
}

int main(void)
{
    one("", "d41d8cd98f00b204e9800998ecf8427e");
    one("a", "0cc175b9c0f1b6a831c399e269772661");
    one("abc", "900150983cd24fb0d6963f7d28e17f72");
    one("message digest", "f96b697d7cb7938d525a2f31aaf161d0");
    one("abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b");
    one("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
        "d174ab98d277d9f5a5611c2c9f419d9f");
    one("1234567890123456789012345678901234567890123456789012345678901234"
        "5678901234567890", "57edf4a22be3c955ac49da2e2107b67a");

    /* a long input crossing many blocks: 1000 x 'a' (value cross-checked
     * with the host md5sum tool) */
    {
        ppp_md5_t m;
        uint8_t d[16], blk[1000];
        char h[33];
        memset(blk, 'a', sizeof blk);
        ppp_md5_init(&m);
        ppp_md5_update(&m, blk, sizeof blk);
        ppp_md5_final(&m, d);
        hex(d, h);
        CHECK(strcmp(h, "cabe45dcc9ae5b66ba86600cca6b8ba8") == 0);
    }
    T_REPORT("test_md5");
}
