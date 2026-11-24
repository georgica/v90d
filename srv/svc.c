/* See svc.h. */
#include <string.h>
#include "svc.h"
#include "bbsdial.h"

static int lower_eq(const char *a, size_t n, const char *b)
{
    size_t i;
    if (strlen(b) != n) return 0;
    for (i = 0; i < n; i++) {
        char c = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] - 'A' + 'a') : a[i];
        if (c != b[i]) return 0;
    }
    return 1;
}

int svc_host_ok(const char *h)
{
    size_t n = strlen(h), i;
    int numeric = 1;
    while (n && h[n - 1] == '.') n--;                   /* "localhost." is localhost too */
    if (n == 0) return 0;
    if (lower_eq(h, n, "localhost") || (n > 10 && lower_eq(h + n - 10, 10, ".localhost"))) return 0;
    for (i = 0; i < n; i++) if (!((h[i] >= '0' && h[i] <= '9') || h[i] == '.')) numeric = 0;
    if (!numeric) {
        /* a name; but 0x7f.0.0.1 and the like are numbers to a resolver */
        if (h[0] == '0' && (h[1] == 'x' || h[1] == 'X')) return 0;
        for (i = 0; i + 1 < n; i++) if ((i == 0 || h[i - 1] == '.') && h[i] == '0' && (h[i + 1] == 'x' || h[i + 1] == 'X')) return 0;
        return 1;
    }
    {
        uint32_t a; char t[16];
        if (n >= sizeof t) return 0;
        memcpy(t, h, n); t[n] = '\0';
        if (cfg_parse_ip(t, &a)) return 0;              /* not four plain decimals: 2130706433, 0177.0.0.1 */
        return bbsdial_is_public(a);
    }
}

int svc_line(const char *line, size_t len, cfg_service_t *e)
{
    size_t i = 0, b;
    if (len && line[len - 1] == '\r') len--;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i == len || line[i] == '#') return SVC_LINE_NONE;
    if (len - i == sizeof SVC_HEAD - 1 && memcmp(line + i, SVC_HEAD, len - i) == 0) return SVC_LINE_HEAD;
    for (b = i; i < len && line[i] != ' ' && line[i] != '\t'; i++) { }
    if (i - b != 3 || memcmp(line + b, "svc", 3) != 0) return SVC_LINE_OTHER;
    {
        const char *why;
        if (cfg_svc_parse(line + i, len - i, e, &why)) return SVC_LINE_BAD;
    }
    if (cfg_svc_number_taken(e->number) || !svc_host_ok(e->host)) return SVC_LINE_BAD;
    return SVC_LINE_SVC;
}
