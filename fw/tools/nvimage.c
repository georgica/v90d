/* See nv.h. The formats v90nv reads and writes. The stock firmware file
 * format is the one fw/tools/mkgsfw.py builds (HARDWARE.md, "Firmware image
 * format"); the AES is written here from FIPS-197, tables computed, because
 * a file for the stock upgrade page comes enciphered and the owner of a
 * box should be able to hand the same file to our own page. */
#include <stdio.h>
#include <string.h>
#include "nv.h"

uint32_t nv_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFul; int k;
    while (n--) { c ^= *p++; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320ul & (0ul - (c & 1))); }
    return ~c;
}
uint32_t nv_le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t nv_be32(const uint8_t *p) { return p[3] | ((uint32_t)p[2] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24); }
void nv_put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }

uint16_t nv_gs_sum(const uint8_t *p, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += le16(p + i);
    return (uint16_t)s;
}

/* ---- U-Boot image ---- */
int nv_uimage_hdr_ok(const uint8_t *h)
{
    uint8_t c[NV_UI_HDR];
    if (nv_be32(h) != NV_UI_MAGIC) return 0;
    memcpy(c, h, NV_UI_HDR); memset(c + 4, 0, 4);
    return nv_crc32(c, NV_UI_HDR) == nv_be32(h + 4);
}

const char *nv_uimage_check(const uint8_t *p, size_t n, size_t *len)
{
    static const char tag[] = NV_IMAGE_TAG;
    uint32_t size; size_t i;
    if (n < NV_UI_HDR || nv_be32(p) != NV_UI_MAGIC) return "this is not a v90d system image";
    if (!nv_uimage_hdr_ok(p)) return "the image header is damaged (its checksum is wrong)";
    if (memchr(p + 32, 0, 32) == 0) return "the image has no proper name";
    if (!strncmp((const char *)p + 32, LD_LOADER_NAME, sizeof LD_LOADER_NAME - 1)) return "this is the loader, not a system image";
    if (strncmp((const char *)p + 32, tag, sizeof tag - 1) || p[32 + sizeof tag - 1] == 0)
        return "the image is not a v90d system for this board (" NV_BOARD ")";
    for (i = 32; p[i]; i++) if (p[i] < 0x20 || p[i] > 0x7E) return "the image has no proper name";
    if (p[28] != 5 || p[29] != 2 || p[30] != 2 || p[31] != 0) return "the image is not an uncompressed ARM Linux kernel image";
    if (nv_be32(p + 16) != LD_LOAD || nv_be32(p + 20) != LD_LOAD) return "the image is built for another load address";
    size = nv_be32(p + 12);
    if (size == 0 || size > NV_IMAGE_MAX) return "the image does not fit the flash partition";
    if (size > n - NV_UI_HDR) return "the image is cut short";
    /* what follows may only be the padding of the firmware file format */
    if (n - NV_UI_HDR - size >= 32) return "the file is longer than the image in it";
    for (i = NV_UI_HDR + size; i < n; i++) if (p[i]) return "the file is longer than the image in it";
    if (nv_crc32(p + NV_UI_HDR, size) != nv_be32(p + 24)) return "the image is damaged (its checksum is wrong)";
    *len = NV_UI_HDR + size;
    return 0;
}

/* ---- AES-128, the inverse cipher only ---- */
static uint8_t sbox[256], inv[256];
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
static uint8_t mul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a)) if (b & 1) r ^= a;
    return r;
}
static void tables(void)
{
    unsigned v, i; uint8_t e[255], l[256], x = 1;
    if (sbox[0]) return;
    for (i = 0; i < 255; i++) { e[i] = x; l[x] = (uint8_t)i; x ^= xt(x); }  /* powers of 3 */
    for (v = 0; v < 256; v++) {
        uint8_t q = v ? e[(255 - l[v]) % 255] : 0, s = q;
        for (i = 1; i < 5; i++) s ^= (uint8_t)((q << i) | (q >> (8 - i)));
        sbox[v] = s ^ 0x63;
    }
    for (v = 0; v < 256; v++) inv[sbox[v]] = (uint8_t)v;
}
static void decrypt_block(const uint8_t rk[176], uint8_t s[16])
{
    int r, i; uint8_t t[16];
    for (i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (r = 9; ; r--) {
        for (i = 0; i < 16; i++) t[(i + 4 * (i % 4)) % 16] = inv[s[i]];    /* rows back, octets back */
        for (i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * r + i];
        if (r == 0) return;
        for (i = 0; i < 16; i += 4) {
            uint8_t a = s[i], b = s[i + 1], c = s[i + 2], d = s[i + 3];
            s[i]     = mul(a, 14) ^ mul(b, 11) ^ mul(c, 13) ^ mul(d, 9);
            s[i + 1] = mul(a, 9) ^ mul(b, 14) ^ mul(c, 11) ^ mul(d, 13);
            s[i + 2] = mul(a, 13) ^ mul(b, 9) ^ mul(c, 14) ^ mul(d, 11);
            s[i + 3] = mul(a, 11) ^ mul(b, 13) ^ mul(c, 9) ^ mul(d, 14);
        }
    }
}
void nv_aes128_decrypt_units(const uint8_t key[16], uint8_t *p, size_t n)
{
    static const uint8_t iv[16] = { 'G', 'r', 'a', 'n', 'd', 's', 't', 'r', 'e', 'a', 'm', ' ', 'I', 'n', 'c', '.' };
    uint8_t rk[176], prev[16], c[16], rcon = 1; size_t at; int i;
    tables();
    memcpy(rk, key, 16);
    for (i = 16; i < 176; i += 4) {
        uint8_t t[4];
        memcpy(t, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = t[0];
            t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
            rcon = xt(rcon);
        }
        rk[i] = rk[i - 16] ^ t[0]; rk[i + 1] = rk[i - 15] ^ t[1]; rk[i + 2] = rk[i - 14] ^ t[2]; rk[i + 3] = rk[i - 13] ^ t[3];
    }
    /* the chain starts again, from the same vector, every two blocks */
    for (at = 0; at + 16 <= n; at += 16) {
        memcpy(c, p + at, 16);
        decrypt_block(rk, p + at);
        for (i = 0; i < 16; i++) p[at + i] ^= (at % 32) ? prev[i] : iv[i];
        memcpy(prev, c, 16);
    }
}

/* The key of the first 32 octets of every file: what the stock upgrader
 * makes of this text when the unit has no key of its own (the same text and
 * the same odd conversion as in mkgsfw.py, which says more about it). It
 * protects nothing; the format has no signature. */
void nv_stock_header_key(uint8_t key[16])
{
    static const char t[] = "37d6ae8bc920374649426438bde35493";
    int i;
#define DIGIT(c) ((unsigned)((c) >= '0' && (c) <= '9' ? (c) - 0x30 : (c) - 0x37))
    for (i = 0; i < 32; i += 4) {
        unsigned hi = DIGIT(t[i + 1]) * 16 + DIGIT(t[i]), lo = DIGIT(t[i + 3]) * 16 + DIGIT(t[i + 2]);
        unsigned w = (hi * 0x100 + lo) & 0xFFFF;
        key[i / 2] = (uint8_t)(w >> 8); key[i / 2 + 1] = (uint8_t)w;
    }
#undef DIGIT
}

/* ---- the stock firmware file ---- */
#define DIR_LEN 0x288u
#define SLOTS   7
static int has_nocase(const char *s, size_t n, const char *word)
{
    size_t k = strlen(word), i, j;
    for (i = 0; i + k <= n && s[i]; i++) {
        for (j = 0; j < k && (s[i + j] | 0x20) == word[j]; j++) ;
        if (j == k) return 1;
    }
    return 0;
}

const char *nv_container_open(uint8_t *p, size_t n, const uint8_t **hdr, const uint8_t **img, size_t *len)
{
    static const char want[] = NV_BOARD "base.bin";
    uint8_t key[16], *f = 0; size_t at = DIR_LEN, fsize = 0, used; int i; const char *why;
    if (n < DIR_LEN || nv_le32(p) != NV_GS_MAGIC) return "this is not a firmware file";
    if (le16(p + 0x206) != 1) return "the firmware file is for another flash layout";
    for (i = 0; i < SLOTS; i++) {
        const char *name = (const char *)p + 4 + 64 * i; uint32_t size = nv_le32(p + 0x1C4 + 4 * i);
        if (!memchr(name, 0, 64)) return "this is not a firmware file";
        if (name[0] && !has_nocase(name, 64, NV_BOARD)) return "the firmware file is for another model";
        if (size > n - at) return "the firmware file is cut short";
        if (size && !strcmp(name, want)) { f = p + at; fsize = size; }
        at += size;
    }
    if (!f) return "the firmware file holds no system for this board (" NV_BOARD ")";
    if (fsize < LD_GS_HDR + NV_UI_HDR || (fsize - LD_GS_HDR) % 16) return "the firmware file is damaged";
    nv_stock_header_key(key);
    nv_aes128_decrypt_units(key, f, 32);
    if (nv_le32(f) != NV_GS_MAGIC) return "the firmware file is damaged, or enciphered for another unit";
    used = nv_le32(f + 12);
    if (used != fsize - LD_GS_HDR || used > LD_BASE_SIZE - LD_GS_HDR) return "the firmware file is damaged";
    if (le16(f + 0x10) != 0x0D) return "the firmware file is damaged";
    if (f[0x34] && strcmp((const char *)f + 0x34, "dvf9918") && strcmp((const char *)f + 0x34, "any"))
        return "the firmware file is for another processor";
    if (!memchr(f + 0x34, 0, 16)) return "the firmware file is damaged";
    if (le16(f + 0x54) > 1) return "the firmware file is for another flash layout";
    for (i = 0; i < 16; i += 2) { key[i] = f[16 + i + 1]; key[i + 1] = f[16 + i]; }
    nv_aes128_decrypt_units(key, f + LD_GS_HDR, used);
    if ((uint16_t)(nv_gs_sum(f + LD_GS_HDR, used) + le16(f + 0x12)) != 0) return "the firmware file is damaged (its checksum is wrong)";
    if (nv_be32(f + LD_GS_HDR) != NV_UI_MAGIC) return "the firmware file is not a v90d release (a stock firmware goes on the stock firmware's own page)";
    if ((why = nv_uimage_check(f + LD_GS_HDR, used, len)) != 0) return why;
    *hdr = f; *img = f + LD_GS_HDR;
    return 0;
}

/* ---- U-Boot's environment: CRC-32 (little endian), NAME=VALUE strings ---- */
#define ENV_DATA (NV_ENV_SIZE - 4)
int nv_env_ok(const uint8_t *env) { return nv_le32(env) == nv_crc32(env + 4, ENV_DATA) && env[NV_ENV_SIZE - 1] == 0; }
const char *nv_env_next(const char *p) { return p + strlen(p) + 1; }
const char *nv_env_find(const uint8_t *env, const char *name)
{
    const char *p; size_t k = strlen(name);
    for (p = (const char *)env + 4; *p; p = nv_env_next(p)) if (!strncmp(p, name, k) && p[k] == '=') return p + k + 1;
    return 0;
}
static size_t env_used(const uint8_t *env)
{
    const char *p = (const char *)env + 4;
    while (*p) p = nv_env_next(p);
    return (size_t)(p - (const char *)env - 4);
}
void nv_env_remove(uint8_t *env, const char *name)
{
    const char *v = nv_env_find(env, name); char *s, *e;
    if (!v) return;
    s = (char *)v - strlen(name) - 1; e = (char *)nv_env_next(s);
    memmove(s, e, (size_t)((char *)env + NV_ENV_SIZE - e));
}
int nv_env_add(uint8_t *env, const char *name, const char *value)
{
    size_t used = env_used(env), need = strlen(name) + 1 + strlen(value) + 1;
    if (!*name || strchr(name, '=') || used + need + 1 > ENV_DATA) return -1;
    sprintf((char *)env + 4 + used, "%s=%s", name, value);
    env[4 + used + need] = 0;
    return 0;
}
void nv_env_seal(uint8_t *env)
{
    size_t used = env_used(env);
    memset(env + 4 + used, 0, ENV_DATA - used);
    nv_put_le32(env, nv_crc32(env + 4, ENV_DATA));
}
