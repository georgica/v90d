/* See vpn.h. */
#include <stdio.h>
#include <string.h>
#include "vpn.h"

/* what a client's profile may say; arguments are OpenVPN's business */
static const char *const allowed[] = {
    "client", "tls-client", "pull", "dev", "dev-type", "proto", "remote", "remote-random", "resolv-retry", "nobind",
    "float", "persist-key", "persist-tun", "cipher", "data-ciphers", "data-ciphers-fallback", "ncp-ciphers", "auth",
    "auth-nocache", "auth-user-pass", "remote-cert-tls", "verify-x509-name", "tls-version-min", "tls-cipher",
    "tls-ciphersuites", "key-direction", "verb", "mute", "mute-replay-warnings", "keepalive", "ping", "ping-restart",
    "reneg-sec", "tun-mtu", "mssfix", "connect-retry", "connect-timeout", "server-poll-timeout",
    "explicit-exit-notify", "compress", "comp-lzo", "allow-compression", "route", "route-delay", "sndbuf", "rcvbuf",
    "fast-io", "tls-timeout", "hand-window", "auth-retry",
};
/* and what may stand in a block */
static const char *const blocks[] = { "ca", "cert", "key", "tls-auth", "tls-crypt", "tls-crypt-v2", "extra-certs", "auth-user-pass" };

static int in_list(const char *const *list, size_t n, const char *w, size_t wn)
{
    size_t i;
    for (i = 0; i < n; i++) if (strlen(list[i]) == wn && memcmp(list[i], w, wn) == 0) return 1;
    return 0;
}

/* a dotted quad at s[0..n): 1 and the address, or 0 */
static int quad(const char *s, size_t n, unsigned long *out)
{
    unsigned long a = 0; size_t i = 0; int part;
    for (part = 0; part < 4; part++) {
        unsigned v = 0; size_t d = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9' && d < 3) { v = v * 10u + (unsigned)(s[i++] - '0'); d++; }
        if (!d || v > 255 || (d > 1 && s[i - d] == '0')) return 0;     /* no leading zero: some read that as octal */
        a = a << 8 | v;
        if (part < 3) { if (i >= n || s[i++] != '.') return 0; }
    }
    if (i != n) return 0;
    *out = a;
    return 1;
}

/* the arguments of a route line at s[0..n): a network inside 10.0.0.0/8,
 * with a mask (if any) of /8 or longer, and whatever follows */
static int route_ok(const char *s, size_t n)
{
    size_t a = 0, e; unsigned long net, mask;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    for (e = a; e < n && s[e] != ' ' && s[e] != '\t'; e++) ;
    if (!quad(s + a, e - a, &net) || (net >> 24) != 10) return 0;
    a = e;
    while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
    if (a == n) return 1;
    for (e = a; e < n && s[e] != ' ' && s[e] != '\t'; e++) ;
    return quad(s + a, e - a, &mask) && (mask >> 24) == 255;
}

static int say(char *msg, size_t cap, int line, const char *fmt, const char *w, size_t wn)
{
    char word[40]; size_t i, k = wn < sizeof word - 1 ? wn : sizeof word - 1;
    /* what is quoted back goes into a page: letters, digits and dashes only */
    for (i = 0; i < k; i++) {
        char c = w[i];
        word[i] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ? c : '?';
    }
    word[k] = 0;
    if (cap) {
        int n = line ? snprintf(msg, cap, "line %d: ", line) : 0;
        if (n >= 0 && (size_t)n < cap) snprintf(msg + n, cap - (size_t)n, fmt, word);
    }
    return -1;
}

int vpn_profile_check(const char *t, size_t len, char *msg, size_t cap)
{
    size_t at = 0; int line = 0, open_line = 0, said = 0, remote = 0; const char *open = NULL; size_t open_n = 0;

    if (len > VPN_PROFILE_MAX) { if (cap) snprintf(msg, cap, "the profile is longer than %d octets", VPN_PROFILE_MAX); return -1; }
    while (at < len) {
        size_t a = at, e = at, w;
        while (e < len && t[e] != '\n') e++;
        at = e < len ? e + 1 : e;
        line++;
        for (w = a; w < e; w++) {
            unsigned char c = (unsigned char)t[w];
            if (!(c >= 32 && c < 127) && c != '\t' && !(c == '\r' && w + 1 == e)) return say(msg, cap, line, "not plain text", "", 0);
        }
        if (e > a && t[e - 1] == '\r') e--;
        while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
        while (e > a && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
        if (open) {                                 /* inside a block: text, until its end */
            if (e - a == open_n + 3 && t[a] == '<' && t[a + 1] == '/' && memcmp(t + a + 2, open, open_n) == 0 && t[e - 1] == '>') open = NULL;
            else if (a < e && t[a] == '<') return say(msg, cap, line, "'<' inside <%s>", open, open_n);
            continue;
        }
        if (a == e || t[a] == '#' || t[a] == ';') continue;
        said = 1;
        if (t[a] == '<') {
            if (t[e - 1] != '>' || e - a < 3 || !in_list(blocks, sizeof blocks / sizeof blocks[0], t + a + 1, e - a - 2))
                return say(msg, cap, line, "a block that is not allowed in a profile here", "", 0);
            open = t + a + 1; open_n = e - a - 2; open_line = line;
            continue;
        }
        for (w = a; w < e && t[w] != ' ' && t[w] != '\t'; w++) ;
        if (!in_list(allowed, sizeof allowed / sizeof allowed[0], t + a, w - a))
            return say(msg, cap, line, "'%s' is not allowed in a profile here", t + a, w - a);
        if (w - a == 14 && memcmp(t + a, "auth-user-pass", 14) == 0 && w < e)
            return say(msg, cap, line, "auth-user-pass may not name a file: give the name and password as a block", "", 0);
        if (w - a == 6 && memcmp(t + a, "remote", 6) == 0 && w < e) remote = 1;
        if (w - a == 5 && memcmp(t + a, "route", 5) == 0 && !route_ok(t + w, e - w))
            return say(msg, cap, line, "a route may only lead into 10.0.0.0/8", "", 0);
    }
    if (open) return say(msg, cap, open_line, "<%s> is not closed", open, open_n);
    if (!said) { if (cap) snprintf(msg, cap, "the profile is empty"); return -1; }
    if (!remote) { if (cap) snprintf(msg, cap, "the profile names no server (remote)"); return -1; }
    return 0;
}

int vpn_profile_remote(const char *t, size_t len, char *out, size_t cap)
{
    size_t at = 0;
    while (at < len) {
        size_t a = at, e = at, n = 0;
        while (e < len && t[e] != '\n') e++;
        at = e < len ? e + 1 : e;
        while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
        if (e - a < 8 || memcmp(t + a, "remote", 6) != 0 || (t[a + 6] != ' ' && t[a + 6] != '\t')) continue;
        a += 7;
        while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
        while (e > a && (t[e - 1] == '\r' || t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
        while (a < e && n + 1 < cap) { char c = t[a++]; out[n++] = c >= 33 && c < 127 && c != '<' && c != '>' && c != '&' && c != '"' ? c : ' '; }
        if (cap) out[n] = 0;
        return 0;
    }
    return -1;
}

int vpn_profile_routes(const char *t, size_t len)
{
    size_t at = 0; int n = 0, in_block = 0;
    while (at < len) {
        size_t a = at, e = at;
        while (e < len && t[e] != '\n') e++;
        at = e < len ? e + 1 : e;
        while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
        if (a < e && t[a] == '<') { in_block = !(a + 1 < e && t[a + 1] == '/'); continue; }
        if (in_block) continue;
        if (e - a >= 5 && memcmp(t + a, "route", 5) == 0 && (e - a == 5 || t[a + 5] == ' ' || t[a + 5] == '\t' || t[a + 5] == '\r')) n++;
    }
    return n;
}
