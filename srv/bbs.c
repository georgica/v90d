/* See bbs.h. */
#include <stdlib.h>
#include <string.h>
#include "bbs.h"

static int is_sp(char c) { return c == ' ' || c == '\t'; }
static int is_dig(char c) { return c >= '0' && c <= '9'; }
static int is_low(char c) { return c >= 'a' && c <= 'z'; }
static int is_host(char c) { return is_dig(c) || is_low(c) || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'; }

/* the next word of p[0..n): its start and length; 0 = there is none */
static size_t word(const char **p, size_t *n, const char **w)
{
    size_t k = 0;
    while (*n && is_sp(**p)) { (*p)++; (*n)--; }
    *w = *p;
    while (k < *n && !is_sp((*p)[k])) k++;
    *p += k; *n -= k;
    return k;
}
static int login_ok(const char *w, size_t k, char out[BBS_LOGIN_MAX + 1])
{
    size_t i;
    if (k < 1 || k > BBS_LOGIN_MAX) return 0;
    for (i = 0; i < k; i++) if (!is_dig(w[i]) && !is_low(w[i])) return 0;
    memcpy(out, w, k); out[k] = 0;
    return 1;
}
/* The box itself is not a board: "localhost", and an address in numbers
 * (as inet_aton reads them: decimal, octal, hex, one to four parts) whose
 * network is 127 or 0. */
static int is_self(const char *h, size_t n)
{
    char b[BBS_HOST_MAX + 1]; char *end; unsigned long v; size_t i;
    for (i = 0; i < n; i++) b[i] = (char)(h[i] >= 'A' && h[i] <= 'Z' ? h[i] - 'A' + 'a' : h[i]);
    b[n] = 0;
    if (strncmp(b, "localhost", 9) == 0 && (b[9] == 0 || b[9] == '.')) return 1;
    if (strspn(b, "0123456789abcdefx.") != n) return 0;        /* a name */
    v = strtoul(b, &end, 0);
    if (end == b || (*end && *end != '.')) return 0;
    if (!*end) v >>= 24;                                        /* one number: all 32 bits */
    return v == 127 || v == 0;
}
/* host[:port] in w[0..k) */
static int address_ok(const char *w, size_t k, bbs_entry *e)
{
    size_t h = 0, i; unsigned long port = 23;
    while (h < k && w[h] != ':') h++;
    if (h < 1 || h > BBS_HOST_MAX || w[0] == '-' || w[0] == '.') return 0;
    for (i = 0; i < h; i++) if (!is_host(w[i])) return 0;
    if (is_self(w, h)) return 0;
    if (h < k) {
        if (k - h - 1 < 1 || k - h - 1 > 5) return 0;
        for (port = 0, i = h + 1; i < k; i++) { if (!is_dig(w[i])) return 0; port = port * 10 + (unsigned long)(w[i] - '0'); }
        if (port < 1 || port > 65535) return 0;
    }
    memcpy(e->host, w, h); e->host[h] = 0; e->port = (uint16_t)port;
    return 1;
}

int bbs_line(const char *line, size_t len, bbs_entry *e)
{
    const char *p = line, *w; size_t k, i, left;
    if (len > BBS_LINE_MAX) return BBS_LINE_BAD;
    /* a comment begins at a '#' that begins the line or follows a space */
    for (i = 0; i < len; i++) if (line[i] == '#' && (i == 0 || is_sp(line[i - 1]))) { len = i; break; }
    while (len && (is_sp(line[len - 1]) || line[len - 1] == '\r')) len--;
    left = len;
    k = word(&p, &left, &w);
    if (k == 0) return BBS_LINE_NONE;
    if (k == 8 && memcmp(w, "v90d-bbs", 8) == 0) {
        k = word(&p, &left, &w);
        return k == 1 && w[0] == '2' && word(&p, &left, &w) == 0 ? BBS_LINE_HEAD : BBS_LINE_BAD;
    }
    if (k == 3 && memcmp(w, "bbs", 3) == 0) {
        memset(e, 0, sizeof *e);
        k = word(&p, &left, &w);
        if (!login_ok(w, k, e->login)) return BBS_LINE_BAD;
        k = word(&p, &left, &w);
        if (k != BBS_NUMBER_LEN || memcmp(w, BBS_AREA, 3) != 0) return BBS_LINE_BAD;
        for (i = 0; i < k; i++) if (!is_dig(w[i])) return BBS_LINE_BAD;
        memcpy(e->number, w, k);
        k = word(&p, &left, &w);
        if (!address_ok(w, k, e)) return BBS_LINE_BAD;
        while (left && is_sp(*p)) { p++; left--; }
        /* the name: what is left, without anything a terminal would act on */
        for (i = 0, k = 0; i < left && k < BBS_NAME_MAX; i++) if (p[i] >= 0x20 && p[i] < 0x7F) e->name[k++] = p[i];
        if (k == 0) strcpy(e->name, e->login);
        return BBS_LINE_BBS;
    }
    return BBS_LINE_BAD;
}

int bbs_number(const char *dialed, char board[BBS_NUMBER_LEN + 1])
{
    size_t n, i;
    if (strcmp(dialed, BBS_NUMBER) == 0 || strcmp(dialed, BBS_NUMBER_LONG) == 0) return BBS_NUM_PROMPT;
    if (dialed[0] == '1' && strlen(dialed) == BBS_NUMBER_LEN + 1) dialed++;
    n = strlen(dialed);
    if (n != BBS_NUMBER_LEN || memcmp(dialed, BBS_AREA, 3) != 0) return BBS_NUM_NONE;
    for (i = 0; i < n; i++) if (!is_dig(dialed[i])) return BBS_NUM_NONE;
    memcpy(board, dialed, n + 1);
    return BBS_NUM_BOARD;
}

void bbs_keypad(const char *login, char out[BBS_NUMBER_LEN + 1])
{
    static const char keys[] = "22233344455566677778889999";       /* a..z */
    size_t k = 3;
    memcpy(out, BBS_AREA, 3);
    for (; *login && k < BBS_NUMBER_LEN; login++) {
        if (is_dig(*login)) out[k++] = *login;
        else if (is_low(*login)) out[k++] = keys[*login - 'a'];
    }
    while (k < BBS_NUMBER_LEN) out[k++] = '0';
    out[k] = 0;
}

void bbs_show(const char *number, char out[BBS_LOGIN_MAX + 1])
{
    size_t i, k = 0;
    for (i = 0; number[i] && k < BBS_LOGIN_MAX - 1; i++) {
        if (i == 3 || i == 6) out[k++] = ' ';
        out[k++] = number[i];
    }
    out[k] = 0;
}

/* letters in lower case and digits of s, at most max of them */
static void key_of(const char *s, char *out, size_t max)
{
    size_t k = 0;
    for (; *s && k < max; s++) {
        if (is_dig(*s) || is_low(*s)) out[k++] = *s;
        else if (*s >= 'A' && *s <= 'Z') out[k++] = (char)(*s - 'A' + 'a');
    }
    out[k] = 0;
}
void bbs_key(const char *typed, char out[BBS_LOGIN_MAX + 1]) { key_of(typed, out, BBS_LOGIN_MAX); }

int bbs_match(const bbs_entry *e, const char *key)
{
    char nk[BBS_NAME_MAX + 1];
    if (!key[0]) return 1;
    if (strstr(e->login, key) || strstr(e->number, key)) return 1;
    key_of(e->name, nk, BBS_NAME_MAX);
    return strstr(nk, key) != NULL;
}

void bbs_edit_init(bbs_edit_t *t) { memset(t, 0, sizeof *t); }

int bbs_edit(bbs_edit_t *t, uint8_t oct, uint8_t echo[3], size_t *echo_n)
{
    *echo_n = 0;
    if (oct == '\r') { t->line[t->n] = 0; t->n = 0; echo[0] = '\r'; echo[1] = '\n'; *echo_n = 2; return 1; }
    if (oct == 8 || oct == 0x7F) {
        if (t->n) { t->n--; echo[0] = 8; echo[1] = ' '; echo[2] = 8; *echo_n = 3; }
        return 0;
    }
    if (oct < 0x20 || oct > 0x7E || t->n >= BBS_TYPED_MAX) return 0;
    t->line[t->n++] = (char)oct;
    echo[0] = oct; *echo_n = 1;
    return 0;
}
