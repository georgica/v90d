/* v90d/cfg - bounded string builder shared by the config writer, the status
 * renderers, the settings form and the HTTP server. Internal header.
 *
 * An sbuf writes into a caller buffer and never past it. One byte is always
 * kept for the terminating NUL. The first append that does not fit sets a
 * sticky overflow flag and nothing more is written, so the buffer holds a
 * clean prefix of the intended text. */
#ifndef V90D_CFG_SBUF_H
#define V90D_CFG_SBUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    int    ovf;
} sbuf_t;

static inline void sb_init(sbuf_t *s, char *buf, size_t cap)
{
    s->p = buf; s->cap = cap; s->len = 0; s->ovf = (cap == 0);
    if (cap) buf[0] = '\0';
}

static inline void sb_putc(sbuf_t *s, char c)
{
    if (s->ovf || s->len + 1 >= s->cap) { s->ovf = 1; return; }
    s->p[s->len++] = c;
}

static inline void sb_puts(sbuf_t *s, const char *z)
{
    while (*z && !s->ovf) sb_putc(s, *z++);
}

static inline void sb_u64(sbuf_t *s, uint64_t v)
{
    char t[20]; int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; } while (v);
    while (n) sb_putc(s, t[--n]);
}

static inline void sb_i32(sbuf_t *s, int32_t v)
{
    uint32_t u = (uint32_t)v;
    if (v < 0) { sb_putc(s, '-'); u = 0u - u; }
    sb_u64(s, u);
}

/* v with exactly `width` digits, zero padded (v must fit) */
static inline void sb_pad(sbuf_t *s, uint32_t v, int width)
{
    char t[10]; int n = 0;
    while (n < width && n < 10) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) sb_putc(s, t[--n]);
}

static inline void sb_hex2(sbuf_t *s, unsigned v)
{
    static const char hx[] = "0123456789abcdef";
    sb_putc(s, hx[(v >> 4) & 15u]);
    sb_putc(s, hx[v & 15u]);
}

/* tenths as a decimal with one fraction digit: -145 -> "-14.5", -5 -> "-0.5" */
static inline void sb_fix1(sbuf_t *s, int32_t tenths)
{
    uint32_t u = (uint32_t)tenths;
    if (tenths < 0) { sb_putc(s, '-'); u = 0u - u; }
    sb_u64(s, u / 10u);
    sb_putc(s, '.');
    sb_putc(s, (char)('0' + (int)(u % 10u)));
}

/* IPv4 address in host byte order as a dotted quad */
static inline void sb_ip(sbuf_t *s, uint32_t a)
{
    int i;
    for (i = 3; i >= 0; i--) {
        sb_u64(s, (a >> (8 * i)) & 255u);
        if (i) sb_putc(s, '.');
    }
}

/* Text from outside for an HTML page, element content or a quoted
 * attribute value: at most max octets of z, stopping at NUL. The five
 * special characters become entities, control octets and DEL \xHH. Octets
 * above 126 become \xHH too, so the output is pure ASCII, unless raw8 is
 * set: then they pass (for text the caller knows to be valid UTF-8). */
static inline void sb_html(sbuf_t *s, const char *z, size_t max, int raw8)
{
    size_t i;
    for (i = 0; i < max && z[i]; i++) {
        unsigned ch = (unsigned char)z[i];
        switch (ch) {
        case '&':  sb_puts(s, "&amp;"); break;
        case '<':  sb_puts(s, "&lt;"); break;
        case '>':  sb_puts(s, "&gt;"); break;
        case '"':  sb_puts(s, "&quot;"); break;
        case '\'': sb_puts(s, "&#39;"); break;
        default:
            if (ch < 0x20 || ch == 0x7f || (ch > 0x7f && !raw8)) { sb_puts(s, "\\x"); sb_hex2(s, ch); }
            else sb_putc(s, (char)ch);
        }
    }
}

/* terminate; length written, or -1 if the text did not fit */
static inline int sb_end(sbuf_t *s)
{
    if (s->cap) s->p[s->len] = '\0';
    return s->ovf ? -1 : (int)s->len;
}

#endif
