/* v90arch - synthetic DNS: the name table and the packet responder. */
#include <stdio.h>
#include <string.h>
#include "dns.h"

static uint32_t name_hash(const char *s)
{
    uint32_t h = 5381;
    while (*s) h = h * 33u + (unsigned char)*s++;
    return h;
}

/* addresses ending in .0 and .255 are left out: old stacks take them for
 * network and broadcast addresses */
static int addr_usable(uint32_t a)
{
    return (a & 0xff) != 0 && (a & 0xff) != 255;
}

int dns_init(dns_t *d, uint32_t base, int prefix)
{
    uint32_t count;

    if (prefix < 8 || prefix > 22) return -1;
    count = (uint32_t)1 << (32 - prefix);
    memset(d, 0, sizeof *d);
    d->base = base & ~(count - 1);
    d->count = count;
    d->next = 1;
    d->ttl = DNS_TTL;
    return 0;
}

int dns_in_pool(const dns_t *d, uint32_t addr)
{
    return addr - d->base < d->count;
}

static dns_ent_t *by_addr(dns_t *d, uint32_t addr)
{
    int i;
    if (addr == 0) return NULL;
    for (i = 0; i < DNS_TAB; i++)
        if (d->e[i].addr == addr) return &d->e[i];
    return NULL;
}

/* lower case, without a final dot; 0 when the name cannot be kept */
static int name_clean(const char *name, char *out)
{
    size_t n = 0;

    for (; *name; name++) {
        unsigned char c = (unsigned char)*name;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return 0;
        if (n >= DNS_NAME_MAX - 1) return 0;
        out[n++] = (char)c;
    }
    if (n > 0 && out[n - 1] == '.') n--;
    out[n] = '\0';
    return n > 0;
}

uint32_t dns_assign(dns_t *d, const char *name)
{
    char clean[DNS_NAME_MAX];
    uint32_t h, a;
    dns_ent_t *v = NULL;
    int i;

    if (!name_clean(name, clean)) return 0;
    h = name_hash(clean);
    for (i = 0; i < DNS_TAB; i++) {
        dns_ent_t *e = &d->e[i];
        if (e->addr == 0) {
            if (v == NULL || v->addr != 0) v = e;      /* a free slot beats any used one */
            continue;
        }
        if (e->hash == h && strcmp(e->name, clean) == 0) {
            e->used = ++d->tick;
            return e->addr;
        }
        if (v == NULL || (v->addr != 0 && (int32_t)(e->used - v->used) < 0)) v = e;
    }
    /* a new name: it takes the next address of the pool that no live name
     * has, so a replaced name's address is not reused before the pool
     * has gone around */
    v->addr = 0;
    do {
        a = d->base + d->next;
        d->next = (d->next + 1) % d->count;
    } while (!addr_usable(a) || by_addr(d, a) != NULL);
    v->addr = a;
    v->hash = h;
    v->used = ++d->tick;
    memcpy(v->name, clean, strlen(clean) + 1);
    return a;
}

const char *dns_name_of(dns_t *d, uint32_t addr)
{
    dns_ent_t *e = dns_in_pool(d, addr) ? by_addr(d, addr) : NULL;
    if (e == NULL) return NULL;
    e->used = ++d->tick;
    return e->name;
}

size_t dns_answer(dns_t *d, const uint8_t *q, size_t qlen, uint8_t *out, size_t cap)
{
    char name[256];
    size_t pos = 12, n = 0, qend;
    unsigned opcode, qtype, qclass, rcode = 0;
    uint32_t addr = 0;

    if (qlen < 12) return 0;
    if (q[2] & 0x80) return 0;                      /* a response */
    if (q[4] != 0 || q[5] != 1) return 0;           /* exactly one question */
    opcode = (q[2] >> 3) & 15;
    for (;;) {
        unsigned l, i;
        if (pos >= qlen) return 0;
        l = q[pos++];
        if (l == 0) break;
        if (l > 63) return 0;                       /* also: no pointers in a question */
        if (pos + l > qlen || n + l + 1 >= sizeof name) return 0;
        for (i = 0; i < l; i++) name[n++] = (char)q[pos++];
        name[n++] = '.';
    }
    name[n] = '\0';
    if (pos + 4 > qlen) return 0;
    qtype = ((unsigned)q[pos] << 8) | q[pos + 1];
    qclass = ((unsigned)q[pos + 2] << 8) | q[pos + 3];
    qend = pos + 4;
    if (cap < qend + 16) return 0;

    if (opcode != 0) {
        rcode = 4;                                  /* not implemented */
    } else if (qtype == 1 && qclass == 1) {
        if (memchr(name, '\0', n) != NULL) addr = 0;
        else addr = dns_assign(d, name);
        if (addr == 0) rcode = 3;                   /* a name we cannot keep: no such name */
    }

    memcpy(out, q, qend);
    out[2] = (uint8_t)(0x80 | (opcode << 3) | 0x04 | (q[2] & 0x01));   /* QR, AA, RD as asked */
    out[3] = (uint8_t)(0x80 | rcode);                                  /* RA */
    out[6] = 0; out[7] = addr ? 1 : 0;
    out[8] = out[9] = out[10] = out[11] = 0;
    if (addr == 0) return qend;
    out[qend + 0] = 0xc0; out[qend + 1] = 0x0c;     /* the name of the question */
    out[qend + 2] = 0; out[qend + 3] = 1;           /* A */
    out[qend + 4] = 0; out[qend + 5] = 1;           /* IN */
    out[qend + 6] = (uint8_t)(d->ttl >> 24); out[qend + 7] = (uint8_t)(d->ttl >> 16);
    out[qend + 8] = (uint8_t)(d->ttl >> 8);  out[qend + 9] = (uint8_t)d->ttl;
    out[qend + 10] = 0; out[qend + 11] = 4;
    out[qend + 12] = (uint8_t)(addr >> 24); out[qend + 13] = (uint8_t)(addr >> 16);
    out[qend + 14] = (uint8_t)(addr >> 8);  out[qend + 15] = (uint8_t)addr;
    return qend + 16;
}

void dns_dotted(uint32_t addr, char *out, size_t cap)
{
    snprintf(out, cap, "%u.%u.%u.%u", (unsigned)(addr >> 24), (unsigned)((addr >> 16) & 255),
             (unsigned)((addr >> 8) & 255), (unsigned)(addr & 255));
}

size_t dns_parse_quad(const char *s, uint32_t *addr)
{
    uint32_t a = 0;
    size_t i = 0;
    int part;

    for (part = 0; part < 4; part++) {
        unsigned v = 0, digits = 0;
        if (part > 0) {
            if (s[i] != '.') return 0;
            i++;
        }
        while (s[i] >= '0' && s[i] <= '9' && digits < 3) { v = v * 10 + (unsigned)(s[i] - '0'); i++; digits++; }
        if (digits == 0 || v > 255) return 0;
        a = (a << 8) | v;
    }
    if (s[i] >= '0' && s[i] <= '9') return 0;
    *addr = a;
    return i;
}

void dns_host_for(dns_t *d, const char *hdr_host, uint32_t orig, char *out, size_t cap)
{
    const char *name;
    uint32_t a;
    size_t n;

    if (cap == 0) return;
    if (hdr_host != NULL && hdr_host[0] != '\0') {
        n = dns_parse_quad(hdr_host, &a);
        if (n > 0 && (hdr_host[n] == '\0' || hdr_host[n] == ':') && (name = dns_name_of(d, a)) != NULL)
            snprintf(out, cap, "%s%s", name, hdr_host + n);
        else
            snprintf(out, cap, "%s", hdr_host);
        return;
    }
    name = dns_name_of(d, orig);
    if (name != NULL) snprintf(out, cap, "%s", name);
    else dns_dotted(orig, out, cap);
}
