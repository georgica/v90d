/* Test data for the compression tests: packet payloads that look like what
 * a dial-up link carries. Everything is generated; no sample files. */
#ifndef V90D_PPP_TEST_GEN_H
#define V90D_PPP_TEST_GEN_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t s; } gen_t;

#if defined(__GNUC__)
#define GEN_U __attribute__((unused))
#else
#define GEN_U
#endif
GEN_U static uint32_t gen_u32(gen_t *g)
{
    uint32_t x = g->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->s = x ? x : 0x1234567u;
    return g->s;
}
GEN_U static unsigned gen_n(gen_t *g, unsigned n) { return (unsigned)((gen_u32(g) >> 8) % n); }

enum { GEN_HTML = 0, GEN_TEXT, GEN_BINARY, GEN_RANDOM, GEN_ZERO, GEN_KINDS };

static const char *const gen_words[] = {
    "the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "he", "was", "for", "on", "are",
    "as", "with", "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by", "word",
    "but", "not", "what", "all", "were", "we", "when", "your", "can", "said", "there", "use", "an",
    "each", "which", "she", "do", "how", "their", "if", "will", "up", "other", "about", "out", "many",
    "then", "them", "these", "so", "some", "her", "would", "make", "like", "him", "into", "time", "has",
    "look", "two", "more", "write", "go", "see", "number", "no", "way", "could", "people", "my", "than",
    "first", "water", "been", "call", "who", "oil", "its", "now", "find", "long", "down", "day", "did",
    "get", "come", "made", "may", "part", "modem", "telephone", "exchange", "subscriber", "connection",
    "information", "network", "protocol", "compression", "dictionary", "transmission", "received"
};
static const char *const gen_tags[] = {
    "<p>", "</p>\r\n", "<a href=\"http://www.example.com/", "\">", "</a>", "<td align=\"left\" valign=\"top\">",
    "</td>", "<tr>", "</tr>\r\n", "<table border=\"0\" cellpadding=\"2\" cellspacing=\"0\" width=\"100%\">\r\n",
    "</table>\r\n", "<font face=\"Arial, Helvetica\" size=\"2\">", "</font>", "<br>\r\n", "<b>", "</b>",
    "<img src=\"/images/", ".gif\" width=\"", "\" height=\"", "\" alt=\"", "<li>", "</li>\r\n", "<h2>", "</h2>\r\n",
    "<div class=\"content\">", "</div>\r\n", "&nbsp;", "<!-- ", " -->\r\n"
};
#define GEN_NWORDS (sizeof gen_words / sizeof gen_words[0])
#define GEN_NTAGS  (sizeof gen_tags / sizeof gen_tags[0])

GEN_U static size_t gen_put(uint8_t *p, size_t at, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l > n - at) l = n - at;
    memcpy(p + at, s, l);
    return at + l;
}

/* n octets of the given kind */
GEN_U static void gen_fill(gen_t *g, int kind, uint8_t *p, size_t n)
{
    size_t at = 0;
    switch (kind) {
    case GEN_HTML:
        while (at < n) {
            unsigned r = gen_n(g, 10);
            if (r < 4) at = gen_put(p, at, n, gen_tags[gen_n(g, GEN_NTAGS)]);
            else {
                at = gen_put(p, at, n, gen_words[gen_n(g, GEN_NWORDS)]);
                if (at < n) p[at++] = r == 9 ? (uint8_t)('0' + gen_n(g, 10)) : ' ';
            }
        }
        break;
    case GEN_TEXT:
        while (at < n) {
            unsigned r = gen_n(g, 40);
            at = gen_put(p, at, n, gen_words[gen_n(g, GEN_NWORDS)]);
            if (at < n) p[at++] = r == 0 ? '.' : r == 1 ? ',' : ' ';
            if (r == 0 && at < n) p[at++] = gen_n(g, 6) ? ' ' : '\n';
        }
        break;
    case GEN_BINARY:
        /* records with counters, small numbers and a few repeated fields */
        while (at < n) {
            uint8_t rec[16];
            uint32_t v = gen_u32(g);
            size_t l = sizeof rec;
            memset(rec, 0, sizeof rec);
            rec[0] = (uint8_t)(at >> 4); rec[1] = 0x80; rec[2] = (uint8_t)(v & 3); rec[3] = 0xff;
            rec[4] = (uint8_t)(v >> 8); rec[5] = (uint8_t)(v >> 16 & 15); rec[8] = 0x45; rec[12] = (uint8_t)(v >> 24 & 1);
            if (l > n - at) l = n - at;
            memcpy(p + at, rec, l);
            at += l;
        }
        break;
    case GEN_RANDOM:
        while (at < n) p[at++] = (uint8_t)(gen_u32(g) >> 11);
        break;
    default:
        memset(p, 0, n);
        break;
    }
}

/* An IP packet as a download brings it: a 40-octet TCP/IP header whose
 * sequence number and checksum move, then payload of the given kind.
 * total >= 40. */
GEN_U static void gen_packet(gen_t *g, int kind, uint32_t seq, uint8_t *p, size_t total)
{
    static const uint8_t hdr[40] = {
        0x45, 0x00, 0, 0, 0, 0, 0x40, 0x00, 0x3e, 0x06, 0, 0, 192, 0, 2, 9, 10, 64, 64, 2,
        0x00, 0x50, 0x04, 0x21, 0, 0, 0, 0, 0x5a, 0x10, 0x33, 0x01, 0x50, 0x10, 0x16, 0xd0, 0, 0, 0, 0 };
    uint32_t ck = gen_u32(g);
    memcpy(p, hdr, 40);
    p[2] = (uint8_t)(total >> 8); p[3] = (uint8_t)total;
    p[4] = (uint8_t)(seq >> 8); p[5] = (uint8_t)seq;
    p[10] = (uint8_t)(ck >> 8); p[11] = (uint8_t)ck;
    p[24] = (uint8_t)(seq >> 24); p[25] = (uint8_t)(seq >> 16); p[26] = (uint8_t)(seq >> 8); p[27] = (uint8_t)seq;
    p[36] = (uint8_t)(ck >> 16); p[37] = (uint8_t)(ck >> 24);
    gen_fill(g, kind, p + 40, total - 40);
}

#endif
