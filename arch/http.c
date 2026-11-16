/* v90arch - HTTP request head handling. */
#include <limits.h>
#include <string.h>
#include "http.h"

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* does the n-byte text s start with the literal, whatever the case */
static int starts(const char *s, size_t n, const char *lit)
{
    size_t i;
    for (i = 0; lit[i]; i++)
        if (i >= n || lower((unsigned char)s[i]) != lower((unsigned char)lit[i])) return 0;
    return 1;
}

/* length of the line at s including its newline, 0 when there is none */
static size_t line_len(const char *s, size_t n)
{
    const char *nl = memchr(s, '\n', n);
    return nl ? (size_t)(nl - s) + 1 : 0;
}

/* the line without CR LF */
static size_t line_text(const char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) len--;
    return len;
}

/* split the request line: method, target and what follows the target */
static int request_line(const char *s, size_t n, size_t *m_len, size_t *t_off, size_t *t_len, size_t *v_off)
{
    size_t i = 0;

    while (i < n && s[i] != ' ' && s[i] != '\t') i++;
    *m_len = i;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    *t_off = i;
    while (i < n && s[i] != ' ' && s[i] != '\t') i++;
    *t_len = i - *t_off;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    *v_off = i;
    return *m_len > 0 && *t_len > 0;
}

size_t http_head_len(const char *b, size_t n)
{
    size_t first = line_len(b, n), m, to, tl, vo, text, pos;

    if (first == 0) return 0;
    text = line_text(b, first);
    request_line(b, text, &m, &to, &tl, &vo);
    if (vo >= text) return first;            /* no version: HTTP/0.9, or nothing usable */
    pos = first;
    for (;;) {
        size_t l = line_len(b + pos, n - pos);
        if (l == 0) return 0;
        pos += l;
        if (line_text(b + pos - l, l) == 0) return pos;
    }
}

/* header name of a line matches (line is name ":" value) */
static int is_header(const char *line, size_t len, const char *name)
{
    size_t nl = strlen(name);
    return len > nl && line[nl] == ':' && starts(line, len, name);
}

static long number(const char *s, size_t n)
{
    long v = 0;
    size_t i = 0, digits = 0;

    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    for (; i < n && s[i] >= '0' && s[i] <= '9'; i++, digits++) {
        if (v > (LONG_MAX - 9) / 10) return -1;
        v = v * 10 + (s[i] - '0');
    }
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return (digits > 0 && i == n) ? v : -1;
}

long http_header_long(const char *head, size_t hlen, const char *name)
{
    size_t pos = line_len(head, hlen);

    while (pos > 0 && pos < hlen) {
        size_t l = line_len(head + pos, hlen - pos), t;
        if (l == 0) break;
        t = line_text(head + pos, l);
        if (is_header(head + pos, t, name))
            return number(head + pos + strlen(name) + 1, t - strlen(name) - 1);
        pos += l;
    }
    return -1;
}

static int host_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']';
}

int http_parse(const char *head, size_t hlen, http_req_t *r)
{
    size_t first = line_len(head, hlen), text, m, to, tl, vo, i, pos;

    memset(r, 0, sizeof *r);
    if (first == 0) return -1;
    text = line_text(head, first);
    if (!request_line(head, text, &m, &to, &tl, &vo)) return -1;
    if (m >= sizeof r->method) return -1;
    memcpy(r->method, head, m);
    if (vo >= text) r->v09 = 1;
    else if (!starts(head + vo, text - vo, "HTTP/")) return -1;
    /* scheme "://" at the start of the target: absolute form */
    for (i = 0; i < tl && ((lower((unsigned char)head[to + i]) >= 'a' && lower((unsigned char)head[to + i]) <= 'z')); i++) ;
    if (i > 0 && i + 3 <= tl && memcmp(head + to + i, "://", 3) == 0) r->absolute = 1;
    else if (head[to] != '/') return -1;     /* CONNECT host:port, OPTIONS * and the like */

    for (pos = first; pos < hlen; ) {
        size_t l = line_len(head + pos, hlen - pos), t;
        const char *line = head + pos;
        if (l == 0) break;
        t = line_text(line, l);
        if (is_header(line, t, "Host")) {
            size_t a = 5, b = t;
            while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
            while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
            r->host[0] = '\0';
            if (b - a < sizeof r->host) {
                for (i = a; i < b && host_char((unsigned char)line[i]); i++) ;
                if (i == b) {
                    memcpy(r->host, line + a, b - a);
                    r->host[b - a] = '\0';
                }
            }
        } else if (is_header(line, t, "Content-Length")) {
            r->body = number(line + 15, t - 15);
            if (r->body < 0) return -1;
        }
        pos += l;
    }
    return 0;
}

static int put(char *out, size_t cap, size_t *n, const char *s, size_t len)
{
    if (len > cap || *n > cap - len) return -1;
    memcpy(out + *n, s, len);
    *n += len;
    return 0;
}

long http_build(const char *head, size_t hlen, const http_req_t *r, const char *host, char *out, size_t cap)
{
    size_t first = line_len(head, hlen), text, m, to, tl, vo, n = 0, pos;
    int have_host = 0, dropping = 0;

    if (first == 0) return -1;
    text = line_text(head, first);
    if (!request_line(head, text, &m, &to, &tl, &vo)) return -1;

    if (r->absolute && !r->v09) {
        if (put(out, cap, &n, head, text)) return -1;
    } else {
        if (put(out, cap, &n, head, m) || put(out, cap, &n, " ", 1)) return -1;
        if (!r->absolute) {
            if (host == NULL || host[0] == '\0') return -1;
            if (put(out, cap, &n, "http://", 7) || put(out, cap, &n, host, strlen(host))) return -1;
        }
        if (put(out, cap, &n, head + to, tl) || put(out, cap, &n, " HTTP/1.0", 9)) return -1;
    }
    if (put(out, cap, &n, "\r\n", 2)) return -1;

    for (pos = first; pos < hlen; ) {
        size_t l = line_len(head + pos, hlen - pos), t;
        const char *line = head + pos;
        if (l == 0) break;
        t = line_text(line, l);
        if (t == 0) break;                   /* the blank line */
        if (line[0] == ' ' || line[0] == '\t') {
            /* continuation: shares the fate of the header it belongs to */
        } else {
            dropping = is_header(line, t, "Connection") || is_header(line, t, "Keep-Alive") ||
                       is_header(line, t, "Proxy-Connection");
            if (is_header(line, t, "Host")) have_host = 1;
        }
        if (!dropping && put(out, cap, &n, line, l)) return -1;
        pos += l;
    }
    if (!have_host && !r->absolute) {
        if (put(out, cap, &n, "Host: ", 6) || put(out, cap, &n, host, strlen(host)) || put(out, cap, &n, "\r\n", 2))
            return -1;
    }
    if (put(out, cap, &n, "Connection: close\r\n\r\n", 21)) return -1;
    return (long)n;
}

long http_resp_head(const char *b, size_t n, int *status)
{
    size_t first, text, i, pos;
    int st = 0, digits = 0;

    if (memcmp(b, "HTTP/", n < 5 ? n : 5) != 0) return -1;
    if (n < 5) return 0;
    first = line_len(b, n);
    if (first == 0) return 0;
    text = line_text(b, first);
    for (i = 5; i < text && b[i] != ' '; i++) ;
    while (i < text && b[i] == ' ') i++;
    for (; i < text && b[i] >= '0' && b[i] <= '9' && digits < 3; i++, digits++) st = st * 10 + (b[i] - '0');
    if (digits != 3) return -1;
    for (pos = first; ; ) {
        size_t l = line_len(b + pos, n - pos);
        if (l == 0) return 0;
        pos += l;
        if (line_text(b + pos - l, l) == 0) break;
    }
    *status = st;
    return (long)pos;
}
