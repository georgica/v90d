/* v90arch - HTML directory listing of the proxy -> "ls -l" lines.
 *
 * An entry is a line that starts (after tags like <PRE> or <HR>) with an
 * anchor and has a date after the anchor:
 *   <A HREF="ftp://host/dir/name"><IMG ...> name</A>   2024-07-18 00:48   1.2K  text
 * The name is taken from the HREF (the visible text may be cut short), a
 * HREF ending in "/" is a directory. Everything else on the page (banner,
 * headings, the link to the parent directory) has no date and is dropped. */
#include <stdio.h>
#include <string.h>
#include "listing.h"

#define NAME_MAX_LEN 255

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int starts(const char *s, size_t n, const char *lit)
{
    size_t i;
    for (i = 0; lit[i]; i++)
        if (i >= n || lower((unsigned char)s[i]) != lit[i]) return 0;
    return 1;
}

/* first place the lower case literal is found in, or n */
static size_t find(const char *s, size_t n, const char *lit)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (starts(s + i, n - i, lit)) return i;
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int digits(const char *s, size_t n)
{
    int v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

unsigned long long lst_size(const char *s, size_t n)
{
    const unsigned long long top = 1ULL << 50;
    unsigned long long whole = 0, frac = 0, den = 1, unit = 1;
    size_t i = 0;

    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        if (whole < top) whole = whole * 10 + (unsigned)(s[i] - '0');
    if (i < n && s[i] == '.') {
        for (i++; i < n && s[i] >= '0' && s[i] <= '9'; i++)
            if (den < 1000000) { frac = frac * 10 + (unsigned)(s[i] - '0'); den *= 10; }
    }
    if (i < n) {
        switch (lower((unsigned char)s[i])) {
        case 'k': unit = 1ULL << 10; break;
        case 'm': unit = 1ULL << 20; break;
        case 'g': unit = 1ULL << 30; break;
        case 't': unit = 1ULL << 40; break;
        default:  return 0;
        }
    }
    if (whole > top / unit * 1024) whole = top / unit * 1024;
    return whole * unit + (frac * unit + den / 2) / den;
}

void lst_init(lst_t *l, int names_only, int now_year, int now_mon)
{
    l->names_only = names_only;
    l->now_year = now_year;
    l->now_mon = now_mon;
    l->skip = 0;
    l->n = 0;
}

size_t lst_entry(const lst_t *l, const char *line, size_t len, char *out, size_t cap)
{
    static const char mon[12][4] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char name[NAME_MAX_LEN + 1], when[8];
    size_t i = 0, h0, h1, e, nn = 0, s0;
    int dir = 0, y, mo, d, hh, mi, age, r;

    /* leading tags other than the anchor */
    for (;;) {
        if (starts(line + i, len - i, "<a ") || starts(line + i, len - i, "<a\t")) break;
        if (i >= len || line[i] != '<') return 0;
        while (i < len && line[i] != '>') i++;
        if (i >= len) return 0;
        i++;
    }
    e = i + find(line + i, len - i, ">");
    h0 = i + find(line + i, e - i, "href=\"");
    if (h0 >= e) return 0;
    h0 += 6;
    for (h1 = h0; h1 < len && line[h1] != '"'; h1++) ;
    if (h1 >= len) return 0;
    e = h1 + find(line + h1, len - h1, "</a>");
    if (e >= len) return 0;
    e += 4;

    /* the date and the size after the anchor */
    while (e < len && line[e] == ' ') e++;
    if (len - e < 16 || line[e + 4] != '-' || line[e + 7] != '-' || line[e + 10] != ' ' || line[e + 13] != ':')
        return 0;
    y = digits(line + e, 4); mo = digits(line + e + 5, 2); d = digits(line + e + 8, 2);
    hh = digits(line + e + 11, 2); mi = digits(line + e + 14, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59) return 0;
    e += 16;
    while (e < len && line[e] == ' ') e++;
    s0 = e;
    while (e < len && line[e] != ' ' && line[e] != '\r') e++;

    /* the name: last part of the HREF path, percent-decoded */
    if (h1 > h0 && line[h1 - 1] == '/') { dir = 1; h1--; }
    i = h0 + find(line + h0, h1 - h0, "://");
    if (i < h1) {
        i += 3;
        while (i < h1 && line[i] != '/') i++;      /* the host */
    } else {
        i = h0;
    }
    for (h0 = i; i < h1; i++)
        if (line[i] == '/') h0 = i + 1;
    for (i = h0; i < h1; i++) {
        int c = (unsigned char)line[i];
        if (c == '%' && i + 2 < h1 && hexval((unsigned char)line[i + 1]) >= 0 && hexval((unsigned char)line[i + 2]) >= 0) {
            c = hexval((unsigned char)line[i + 1]) * 16 + hexval((unsigned char)line[i + 2]);
            i += 2;
        } else if (c == '&' && starts(line + i, h1 - i, "&amp;")) {
            i += 4;
        }
        if (c < 32 || c == 127 || c == '/') return 0;
        if (nn >= NAME_MAX_LEN) return 0;
        name[nn++] = (char)c;
    }
    name[nn] = '\0';
    if (nn == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;

    if (l->names_only) {
        r = snprintf(out, cap, "%s\r\n", name);
    } else {
        /* like ls: the time for the last six months, else the year */
        age = (l->now_year * 12 + l->now_mon) - (y * 12 + mo);
        if (l->now_year > 0 && age >= 0 && age < 6) snprintf(when, sizeof when, "%02d:%02d", hh % 100, mi % 100);
        else snprintf(when, sizeof when, "%5d", y % 100000);
        r = snprintf(out, cap, "%s   1 ftp      ftp      %10llu %s %2d %5s %s\r\n",
                     dir ? "drwxr-xr-x" : "-rw-r--r--", dir ? 0ULL : lst_size(line + s0, e - s0),
                     mon[mo - 1], d, when, name);
    }
    if (r < 0 || (size_t)r >= cap) return 0;
    return (size_t)r;
}

size_t lst_feed(lst_t *l, const char *in, size_t n, char *out, size_t cap, size_t *outlen)
{
    size_t i, o = 0;

    for (i = 0; i < n; i++) {
        char c = in[i];
        if (c == '\n') {
            if (cap - o < LST_OUT_MAX) break;       /* no room for a line: come back later */
            if (!l->skip) o += lst_entry(l, l->line, l->n, out + o, cap - o);
            l->n = 0;
            l->skip = 0;
        } else if (l->n < LST_LINE_MAX) {
            l->line[l->n++] = c;
        } else {
            l->skip = 1;
        }
    }
    *outlen = o;
    return i;
}

size_t lst_finish(lst_t *l, char *out, size_t cap)
{
    size_t o = 0;
    if (!l->skip && l->n > 0) o = lst_entry(l, l->line, l->n, out, cap);
    l->n = 0;
    l->skip = 0;
    return o;
}
