/* See dcnow.h. */
#include <string.h>
#include "dcnow.h"

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a[8], t1, t2; int i;
    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7]
             + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
    memcpy(a, h, sizeof a);
    for (i = 0; i < 64; i++) {
        t1 = a[7] + (ror(a[4], 6) ^ ror(a[4], 11) ^ ror(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        t2 = (ror(a[0], 2) ^ ror(a[0], 13) ^ ror(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1;
        a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
    }
    for (i = 0; i < 8; i++) h[i] += a[i];
}

void sha256_hex(const void *data, size_t len, char out[65])
{
    static const char hx[] = "0123456789abcdef";
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data; uint8_t last[128]; size_t n = len, tail; uint64_t bits = (uint64_t)len * 8; int i;

    while (n >= 64) { block(h, p); p += 64; n -= 64; }
    memset(last, 0, sizeof last);
    if (n) memcpy(last, p, n);
    last[n] = 0x80;
    tail = n < 56 ? 64 : 128;
    for (i = 0; i < 8; i++) last[tail - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    block(h, last);
    if (tail == 128) block(h, last + 64);
    for (i = 0; i < 32; i++) {
        unsigned b = (h[i / 4] >> (24 - 8 * (i % 4))) & 0xff;
        out[2 * i] = hx[b >> 4]; out[2 * i + 1] = hx[b & 15];
    }
    out[64] = 0;
}

void dcnow_start(dcnow_t *d, const uint8_t mac[6])
{
    static const char hx[] = "0123456789ABCDEF";
    char text[18]; int i;
    memset(d, 0, sizeof *d);
    for (i = 0; i < 6; i++) { text[3 * i] = hx[mac[i] >> 4]; text[3 * i + 1] = hx[mac[i] & 15]; text[3 * i + 2] = ':'; }
    text[17] = 0;
    memcpy(d->path, "/api/update/", 12);
    sha256_hex(text, 17, d->path + 12);
    d->path[76] = '/'; d->path[77] = 0;
}

const char *dcnow_path(const dcnow_t *d) { return d->path; }

int dcnow_body(dcnow_t *d, const char *name, char *out, size_t cap)
{
    static const char *const once[2] = { "gameloft", "onsen0.overworks.isao.net" };
    int i;
    if (cap) out[0] = 0;
    if (name[0] && !strstr(name, "appspot") && strlen(name) < sizeof d->last) strcpy(d->last, name);
    if (!d->last[0] || cap < 10 + 64 + 1) return 0;
    for (i = 0; i < 2; i++) if (strstr(d->last, once[i])) {
        if (d->once[i]) return 0;
        d->once[i] = 1;
    }
    memcpy(out, "dns_query=", 10);
    sha256_hex(d->last, strlen(d->last), out + 10);
    return 74;
}
