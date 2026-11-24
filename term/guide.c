/* See guide.h. */
#include <string.h>
#include "guide.h"

static void trim(const char **s, size_t *n)
{
    while (*n && ((*s)[0] == ' ' || (*s)[0] == '\t')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t' || (*s)[*n - 1] == '\r')) (*n)--;
}

static void copy(char *dst, size_t cap, const char *s, size_t n)
{
    trim(&s, &n);
    if (n >= cap) n = cap - 1;
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* "Key:" at the start of s[0..n) (after blanks): the rest of the line */
static int field(const char *s, size_t n, const char *key, const char **v, size_t *vn)
{
    size_t k = strlen(key);
    trim(&s, &n);
    if (n < k + 1 || memcmp(s, key, k) != 0 || s[k] != ':') return 0;
    *v = s + k + 1; *vn = n - k - 1;
    trim(v, vn);
    return 1;
}

static const char *find(const char *s, size_t n, const char *what)
{
    size_t k = strlen(what), i;
    for (i = 0; i + k <= n; i++) if (memcmp(s + i, what, k) == 0) return s + i;
    return NULL;
}

static int all_of(const char *s, size_t n, char c, size_t at_least)
{
    size_t i, k = 0;
    for (i = 0; i < n; i++) { if (s[i] == c) k++; else if (s[i] != ' ' && s[i] != '\r') return 0; }
    return k >= at_least;
}

int gd_date(const char *s, char out[11])
{
    static const char *const M[12] = { "January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December" };
    int m, d = 0, y = 0, i;
    size_t k;
    out[0] = '\0';
    while (*s == ' ') s++;
    for (m = 0; m < 12; m++) { k = strlen(M[m]); if (strncmp(s, M[m], k) == 0 && s[k] == ' ') break; }
    if (m == 12) return 0;
    s += strlen(M[m]) + 1;
    for (i = 0; i < 2 && *s >= '0' && *s <= '9'; i++, s++) d = d * 10 + (*s - '0');
    if (i == 0 || d < 1 || d > 31 || *s++ != ',') return 0;
    while (*s == ' ') s++;
    for (i = 0; i < 4 && *s >= '0' && *s <= '9'; i++, s++) y = y * 10 + (*s - '0');
    if (i != 4 || y < 1980) return 0;
    out[0] = (char)('0' + y / 1000); out[1] = (char)('0' + y / 100 % 10); out[2] = (char)('0' + y / 10 % 10); out[3] = (char)('0' + y % 10);
    out[4] = '-'; out[5] = (char)('0' + (m + 1) / 10); out[6] = (char)('0' + (m + 1) % 10);
    out[7] = '-'; out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10); out[10] = '\0';
    return 1;
}

void gd_init(gd_t *g) { memset(g, 0, sizeof *g); }

static void desc_add(gd_entry *e, const char *s, size_t n)
{
    size_t have = strlen(e->desc);
    trim(&s, &n);
    if (n == 0) {                             /* a blank line: one between paragraphs, none at the start */
        if (have == 0 || e->desc[have - 1] == '\n') return;
        if (have + 1 < sizeof e->desc) { e->desc[have] = '\n'; e->desc[have + 1] = '\0'; }
        return;
    }
    /* after text a line break; after a blank line (a '\n' already) a second one */
    if (have && have + 1 < sizeof e->desc) e->desc[have++] = '\n';
    if (have + n >= sizeof e->desc) n = sizeof e->desc - 1 - have;
    memcpy(e->desc + have, s, n);
    e->desc[have + n] = '\0';
}

/* the Location: "Madrid, , Spain" reads better as "Madrid, Spain" */
static void tidy_place(char *p)
{
    char *a;
    while ((a = strstr(p, ", ,")) != NULL) memmove(a, a + 2, strlen(a + 2) + 1);
    while (p[0] == ',' || p[0] == ' ') memmove(p, p + 1, strlen(p));
    while (*p && (p[strlen(p) - 1] == ',' || p[strlen(p) - 1] == ' ')) p[strlen(p) - 1] = '\0';
}

static int finish(gd_t *g, gd_entry *out)
{
    size_t n;
    int ok = g->have && g->dated && g->e.name[0] && g->e.host[0];
    if (ok) {
        n = strlen(g->e.desc);
        while (n && g->e.desc[n - 1] == '\n') g->e.desc[--n] = '\0';
        tidy_place(g->e.loc);
        *out = g->e;
    }
    memset(&g->e, 0, sizeof g->e);
    g->have = g->after_sw = g->dated = 0;
    return ok;
}

int gd_line(gd_t *g, const char *line, size_t len, gd_entry *out)
{
    const char *v, *t = line;
    size_t vn, tn = len;

    if (all_of(line, len, '-', 40)) { int r = finish(g, out); g->in = 1; return r; }
    if (all_of(line, len, '=', 40)) { int r = finish(g, out); g->in = 0; return r; }
    if (!g->in) return 0;
    trim(&t, &tn);
    if (!g->have) {                           /* the first line of an entry is its name */
        if (tn == 0) return 0;
        if (t[0] == '*') { t++; tn--; trim(&t, &tn); }
        copy(g->e.name, sizeof g->e.name, t, tn);
        g->have = 1;
        return 0;
    }
    if (g->after_sw) { desc_add(&g->e, line, len); return 0; }
    if (field(line, len, "Last Updated", &v, &vn)) {
        char d[64];
        g->dated = 1;
        copy(d, sizeof d, v, vn);
        (void)gd_date(d, g->e.upd);
    } else if (field(line, len, "Telnet", &v, &vn)) {
        const char *c = memchr(v, ':', vn);
        unsigned long p = 0;
        size_t hn = c ? (size_t)(c - v) : vn, i;
        if (c) for (i = hn + 1; i < vn && v[i] >= '0' && v[i] <= '9' && p < 65536; i++) p = p * 10 + (unsigned long)(v[i] - '0');
        copy(g->e.host, sizeof g->e.host, v, hn);
        for (i = 0; g->e.host[i]; i++) if (g->e.host[i] >= 'A' && g->e.host[i] <= 'Z') g->e.host[i] = (char)(g->e.host[i] + 32);
        g->e.port = (uint16_t)(p > 0 && p < 65536 ? p : 23);
    } else if (field(line, len, "WEB", &v, &vn)) {
        copy(g->e.web, sizeof g->e.web, v, vn);
    } else if (field(line, len, "Location", &v, &vn)) {
        copy(g->e.loc, sizeof g->e.loc, v, vn);
    } else if (field(line, len, "Software", &v, &vn)) {
        /* "Synchronet          Total Nodes: 4     Login: NEW" */
        const char *tot = find(v, vn, "Total Nodes:"), *lg = find(v, vn, "Login:");
        const char *end = tot ? tot : lg ? lg : v + vn;
        copy(g->e.sw, sizeof g->e.sw, v, (size_t)(end - v));
        if (tot) {
            const char *a = tot + 12, *b = lg && lg > tot ? lg : v + vn;
            size_t i, k = 0;
            char nd[sizeof g->e.nodes];
            copy(nd, sizeof nd, a, (size_t)(b - a));
            for (i = 0; nd[i] && nd[i] >= '0' && nd[i] <= '9'; i++) g->e.nodes[k++] = nd[i];
            g->e.nodes[k] = '\0';
        }
        if (lg) copy(g->e.login, sizeof g->e.login, lg + 6, (size_t)(v + vn - (lg + 6)));
        g->after_sw = 1;
    }
    return 0;
}

int gd_end(gd_t *g, gd_entry *out) { return finish(g, out); }

/* ---- wrapping ----------------------------------------------------------- */

static size_t cp_len(const char *s)          /* octets of the UTF-8 character at s */
{
    unsigned char c = (unsigned char)s[0];
    size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1, i;
    for (i = 1; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) return 1;
    return n;
}

int gd_wrap(const char *text, int cols, int max_lines, void (*line)(void *ctx, const char *s, size_t n), void *ctx)
{
    int lines = 0;
    const char *p = text;
    while (*p && (max_lines <= 0 || lines < max_lines)) {
        const char *para = p, *pend = strchr(p, '\n');
        if (!pend) pend = p + strlen(p);
        if (para == pend) { line(ctx, p, 0); lines++; }
        while (para < pend && (max_lines <= 0 || lines < max_lines)) {
            const char *s = para, *brk = NULL, *q = para;
            int w = 0;
            while (*s == ' ' && s < pend) s++;
            q = s;
            while (q < pend && w < cols) {        /* as much as fits */
                if (*q == ' ') brk = q;
                q += cp_len(q); w++;
            }
            if (q < pend && *q != ' ' && brk) q = brk;     /* back to the last space */
            {
                const char *e = q;
                while (e > s && e[-1] == ' ') e--;
                if (e > s || q >= pend) { line(ctx, s, (size_t)(e - s)); lines++; }
            }
            para = q;
            while (para < pend && *para == ' ') para++;
        }
        p = *pend ? pend + 1 : pend;
    }
    return lines;
}

/* ---- the list ----------------------------------------------------------- */

typedef struct { char *p; size_t cap, n; } wb_t;
static void wb_put(wb_t *w, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++, w->n++) if (w->n + 1 < w->cap) w->p[w->n] = s[i];
    if (w->cap) w->p[w->n < w->cap ? w->n : w->cap - 1] = '\0';
}
static void wb_kv(wb_t *w, const char *k, const char *v)
{
    if (!v[0]) return;
    wb_put(w, k, strlen(k)); wb_put(w, " ", 1); wb_put(w, v, strlen(v)); wb_put(w, "\n", 1);
}
static void wb_desc(void *ctx, const char *s, size_t n)
{
    wb_t *w = ctx;
    wb_put(w, "d ", n ? 2 : 1); wb_put(w, s, n); wb_put(w, "\n", 1);
}

size_t bi_write(const gd_entry *e, const char *login, char *out, size_t cap)
{
    wb_t w = { out, cap, 0 };
    wb_kv(&w, "board", login);
    wb_kv(&w, "loc", e->loc);
    wb_kv(&w, "sw", e->sw);
    wb_kv(&w, "nodes", e->nodes);
    wb_kv(&w, "new", e->login);
    wb_kv(&w, "web", e->web);
    wb_kv(&w, "upd", e->upd);
    if (e->desc[0]) (void)gd_wrap(e->desc, BI_DESC_COLS, BI_DESC_LINES, wb_desc, &w);
    return w.n;
}

int bi_line(const char *line, size_t len, const char **key, size_t *klen, const char **val, size_t *vlen)
{
    size_t i, k;
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (len == 0 || line[0] == '#') return BI_NONE;
    if (len == strlen(BI_HEAD) && memcmp(line, BI_HEAD, len) == 0) return BI_HEAD_LINE;
    for (k = 0; k < len && line[k] >= 'a' && line[k] <= 'z'; k++) ;
    if (k == 0 || (k < len && line[k] != ' ')) return BI_BAD;
    *key = line; *klen = k;
    *val = k < len ? line + k + 1 : line + k;
    *vlen = k < len ? len - k - 1 : 0;
    if (k == 5 && memcmp(line, "board", 5) == 0) {
        if (*vlen == 0 || *vlen > BI_LOGIN_MAX) return BI_BAD;
        for (i = 0; i < *vlen; i++)
            if (!(((*val)[i] >= 'a' && (*val)[i] <= 'z') || ((*val)[i] >= '0' && (*val)[i] <= '9'))) return BI_BAD;
        return BI_BOARD;
    }
    return BI_FIELD;
}
