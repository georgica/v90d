/* See tdir.h. */
#include <string.h>
#include "tdir.h"
#include "guide.h"

void tdir_init(tdir_t *d)
{
    d->n = 0;
    d->pool[0] = '\0';
    d->used = 1;                                /* offset 0: the empty string */
    memset(d->hash, 0, sizeof d->hash);
    d->cur = -1;
    d->with_info = 0;
    d->sorted = 0;
}

static uint32_t keep(tdir_t *d, const char *s, size_t n)
{
    uint32_t at = d->used;
    if (n == 0) return 0;
    if (d->used + n + 1 > TDIR_POOL) return 0;
    memcpy(d->pool + at, s, n);
    d->pool[at + n] = '\0';
    d->used += (uint32_t)n + 1;
    return at;
}

static unsigned hash_of(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h & (TDIR_HASH - 1);
}

static int lookup(const tdir_t *d, const char *login, size_t n)
{
    unsigned h = hash_of(login, n), k;
    for (k = 0; k < TDIR_HASH; k++, h = (h + 1) & (TDIR_HASH - 1)) {
        int i = d->hash[h] - 1;
        const char *l;
        if (i < 0) return -1;
        l = d->pool + d->b[i].f[TD_LOGIN];
        if (strlen(l) == n && memcmp(l, login, n) == 0) return i;
    }
    return -1;
}

/* the next word of s[0..n) from *at */
static int word(const char *s, size_t n, size_t *at, const char **w, size_t *wn)
{
    while (*at < n && s[*at] == ' ') (*at)++;
    *w = s + *at;
    while (*at < n && s[*at] != ' ') (*at)++;
    *wn = (size_t)(s + *at - *w);
    return *wn > 0;
}

void tdir_bbs_line(tdir_t *d, const char *s, size_t n)
{
    const char *w, *login, *num;
    size_t wn, ln, nn, at = 0, i;
    tdir_board *b;
    unsigned h;
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
    if (!word(s, n, &at, &w, &wn) || wn != 3 || memcmp(w, "bbs", 3) != 0) return;
    if (!word(s, n, &at, &login, &ln) || ln > BI_LOGIN_MAX) return;
    if (!word(s, n, &at, &num, &nn) || nn != 10) return;
    for (i = 0; i < nn; i++) if (num[i] < '0' || num[i] > '9') return;
    if (!word(s, n, &at, &w, &wn)) return;                  /* the host: not ours to keep */
    while (at < n && s[at] == ' ') at++;
    if (at >= n || d->n >= TDIR_MAX || lookup(d, login, ln) >= 0) return;
    b = &d->b[d->n];
    memset(b, 0, sizeof *b);
    b->f[TD_LOGIN] = keep(d, login, ln);
    b->f[TD_NUMBER] = keep(d, num, nn);
    b->f[TD_NAME] = keep(d, s + at, n - at);
    if (!b->f[TD_LOGIN] || !b->f[TD_NAME]) return;          /* the pool is full */
    for (h = hash_of(login, ln); d->hash[h]; h = (h + 1) & (TDIR_HASH - 1)) ;
    d->hash[h] = (uint16_t)(d->n + 1);
    d->order[d->n] = (uint16_t)d->n;
    d->n++;
    d->sorted = 0;
}

void tdir_info_line(tdir_t *d, const char *s, size_t n)
{
    static const char *const KEY[] = { "loc", "sw", "nodes", "new", "web", "upd" };
    static const int FIELD[] = { TD_LOC, TD_SW, TD_NODES, TD_NEW, TD_WEB, TD_UPD };
    const char *k, *v;
    size_t kn, vn, i;
    tdir_board *b;
    int t = bi_line(s, n, &k, &kn, &v, &vn);

    if (t == BI_BOARD) {
        d->cur = lookup(d, v, vn);
        if (d->cur >= 0) d->with_info++;
        return;
    }
    if (t != BI_FIELD || d->cur < 0) return;
    b = &d->b[d->cur];
    if (kn == 1 && k[0] == 'd') {                           /* a line of the description */
        uint32_t at = b->f[TD_DESC];
        size_t have = at ? strlen(d->pool + at) : 0;
        if (at && at + have + 1 != d->used) {               /* not at the end of the pool: move it there */
            uint32_t to = keep(d, d->pool + at, have);
            if (!to) return;
            b->f[TD_DESC] = at = to;
        }
        /* The lines are a paragraph wrapped for the list: joined again with
         * a space, so that the page wraps it as it needs; an empty "d" line
         * ends a paragraph, and a blank line stands between it and the next
         * ("A\n\nB": gd_wrap's paragraphs) */
        if (!at) { if (vn) b->f[TD_DESC] = keep(d, v, vn); return; }
        if (vn == 0) {
            if (have && d->pool[at + have - 1] != '\n' && d->used + 1 <= TDIR_POOL) {
                d->pool[d->used - 1] = '\n'; d->pool[d->used] = '\0'; d->used++;
            }
            return;
        }
        if (d->used + vn + 2 > TDIR_POOL) return;
        {
            int after_break = have && d->pool[at + have - 1] == '\n';
            d->pool[d->used - 1] = after_break ? '\n' : ' ';   /* its NUL becomes the joint */
            memcpy(d->pool + d->used, v, vn);
            d->used += (uint32_t)vn + 1;
            d->pool[d->used - 1] = '\0';
        }
        return;
    }
    for (i = 0; i < sizeof KEY / sizeof KEY[0]; i++)
        if (strlen(KEY[i]) == kn && memcmp(k, KEY[i], kn) == 0) { b->f[FIELD[i]] = keep(d, v, vn); return; }
    /* a key this build does not know: passed over */
}

const char *tdir_get(const tdir_t *d, int i, int f)
{
    if (i < 0 || i >= d->n || f < 0 || f >= TD_FIELDS) return "";
    return d->pool + d->b[i].f[f];
}

char tdir_file_as(const char *name)
{
    char c = name[0];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return c >= 'A' && c <= 'Z' ? c : '#';
}

static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

/* q (lower case) somewhere in s, letters compared without case */
static int has(const char *s, const char *q, size_t qn)
{
    for (; *s; s++) {
        size_t i;
        for (i = 0; i < qn && s[i] && low(s[i]) == q[i]; i++) ;
        if (i == qn) return 1;
    }
    return 0;
}

void tdir_search(tdir_view *v, const char *q)
{
    size_t i;
    for (i = 0; q[i] && i < TDIR_Q_MAX; i++) v->q[i] = low(q[i]);
    v->q[i] = '\0';
    v->nres = v->scanned = 0;
}

static int name_cmp(const tdir_t *d, int a, int b)
{
    const char *x = tdir_get(d, a, TD_NAME), *y = tdir_get(d, b, TD_NAME);
    for (; *x && low(*x) == low(*y); x++, y++) ;
    return (unsigned char)low(*x) - (unsigned char)low(*y);
}

void tdir_sort(tdir_t *d)
{
    static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };   /* Ciura's */
    size_t g;
    int i, j;
    for (i = 0; i < d->n; i++) d->order[i] = (uint16_t)i;
    for (g = 0; g < sizeof gaps / sizeof gaps[0]; g++) {
        int gap = gaps[g];
        for (i = gap; i < d->n; i++) {
            uint16_t t = d->order[i];
            for (j = i; j >= gap && name_cmp(d, d->order[j - gap], t) > 0; j -= gap) d->order[j] = d->order[j - gap];
            d->order[j] = t;
        }
    }
    d->sorted = 1;
}

int tdir_step(const tdir_t *d, tdir_view *v, int budget)
{
    size_t qn = strlen(v->q);
    while (budget-- > 0 && v->scanned < d->n) {
        int i = d->order[v->scanned++];
        if (!qn || has(tdir_get(d, i, TD_NAME), v->q, qn) || has(tdir_get(d, i, TD_LOC), v->q, qn) ||
            has(tdir_get(d, i, TD_SW), v->q, qn) || has(tdir_get(d, i, TD_DESC), v->q, qn))
            v->res[v->nres++] = (uint16_t)i;
    }
    return v->scanned >= d->n;
}

int tdir_done(const tdir_t *d, const tdir_view *v) { return v->scanned >= d->n; }

int tdir_letter(const tdir_t *d, const tdir_view *v, char letter)
{
    int r;
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 32);
    for (r = 0; r < v->nres; r++) if (tdir_file_as(tdir_get(d, v->res[r], TD_NAME)) == letter) return r;
    return -1;
}

int tdir_find(const tdir_t *d, const char *typed)
{
    char key[64], dig[16];
    size_t k = 0, nd = 0;
    int i, other = 0;
    const char *p;
    for (p = typed; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (k < sizeof key - 1) key[k++] = c; }
        if (c >= '0' && c <= '9') { if (nd < sizeof dig - 1) dig[nd++] = c; else other = 1; }
        else if (c != ' ' && c != '-' && c != '(' && c != ')' && c != '.') other = 1;
    }
    key[k] = dig[nd] = '\0';
    if (!k) return -1;
    i = lookup(d, key, k);
    if (i >= 0) return i;
    if (other) return -1;
    p = dig;
    if (nd == 11 && dig[0] == '1') p++;
    if (strlen(p) != 10) return -1;
    for (i = 0; i < d->n; i++) if (strcmp(tdir_get(d, i, TD_NUMBER), p) == 0) return i;
    return -1;
}
