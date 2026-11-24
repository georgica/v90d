/* What both flash tools' web halves share: v90nv's (the HT80x, nvhttp.c)
 * and obinv's (the OBi200). They are started by the daemon with the
 * browser's connection as standard input and output, read the posted body,
 * send one page in the boxes' look (cfg/page.h) and leave notes for the
 * Firmware page and the boot script. Static functions: include it once,
 * after sbuf.h and page.h, with RECV_S (seconds a silent browser is given)
 * defined. Ours; written for this project. */
#ifndef V90D_FWSERVE_H
#define V90D_FWSERVE_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

static const char *env_or(const char *name, const char *dflt) { const char *v = getenv(name); return v && v[0] ? v : dflt; }

static const char *reason(int code)
{
    switch (code) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 409: return "Conflict";
    case 413: return "Request Entity Too Large";
    default:  return "Internal Server Error";
    }
}

static void send_all(const char *p, size_t n)
{
    while (n) {
        ssize_t r = write(1, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;                     /* the browser is gone; what we do does not depend on it */
        p += r; n -= (size_t)r;
    }
}

/* one page: a headline word, a paragraph, and the way back */
static void answer(int code, const char *badge, const char *text, const char *detail, int refresh_s)
{
    static char body[16384], head[640], meta[96];
    sbuf_t b, h;
    meta[0] = 0;
    if (refresh_s) snprintf(meta, sizeof meta, "<meta http-equiv=\"refresh\" content=\"%d;url=/firmware\">\n", refresh_s);
    sb_init(&b, body, sizeof body);
    page_top(&b, "firmware", meta, PAGE_TAB_FIRMWARE, NULL);
    sb_puts(&b, "<section class=\"hero\"><span class=\"b s1\">"); sb_puts(&b, badge);
    sb_puts(&b, "</span>\n<p class=\"sum\">"); sb_html(&b, text, 2000, 0); sb_puts(&b, "</p>\n");
    if (detail && detail[0]) { sb_puts(&b, "<p>"); sb_html(&b, detail, 2000, 0); sb_puts(&b, "</p>\n"); }
    sb_puts(&b, "<p><a href=\"/firmware\">Back to the Firmware page</a></p></section>\n" PAGE_END);
    if (sb_end(&b) < 0) return;
    sb_init(&h, head, sizeof head);
    sb_puts(&h, "HTTP/1.0 "); sb_u64(&h, (uint64_t)code); sb_putc(&h, ' '); sb_puts(&h, reason(code));
    sb_puts(&h, "\r\nServer: v90d\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "); sb_u64(&h, b.len);
    sb_puts(&h, "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'\r\n\r\n");
    if (sb_end(&h) < 0) return;
    send_all(head, h.len);
    send_all(body, b.len);
}

/* our side of the connection is done: the browser has its page */
static void hang_up(void)
{
    char junk[512]; struct timeval tv = { 1, 0 }; int i;
    shutdown(1, SHUT_WR);
    setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (i = 0; i < 8 && read(0, junk, sizeof junk) > 0; i++) ;
    close(0); close(1);
}

static void note_state(const char *text)
{
    const char *path = env_or("V90NV_STATE", "/tmp/v90d.fwstate"); char tmp[300];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "%s\n", text);
    if (fclose(f) == 0) rename(tmp, path);
}

static int ask_restart(void)
{
    int fd = open(env_or("V90NV_RESTART_NOTE", "/tmp/v90d.restart"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

/* the body: first what the daemon already had, then the connection */
static long read_body(uint8_t *p, size_t want)
{
    const char *e = getenv("V90NV_EARLY_FD"); size_t got = 0; int fd = e ? atoi(e) : -1, fl;
    struct timeval tv = { RECV_S, 0 };
    if (fd > 2) {
        while (got < want) {
            ssize_t r = read(fd, p + got, want - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    fl = fcntl(0, F_GETFL);
    if (fl >= 0) fcntl(0, F_SETFL, fl & ~O_NONBLOCK);
    setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got < want) {
        ssize_t r = read(0, p + got, want - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (long)got;
}

static const uint8_t *find(const uint8_t *p, size_t n, const char *what, size_t k)
{
    size_t i;
    for (i = 0; i + k <= n; i++) if (p[i] == (uint8_t)what[0] && !memcmp(p + i, what, k)) return p + i;
    return 0;
}

/* The file inside a posted form (RFC 7578): the part whose name is "image",
 * or the only part there is. 0 if the body is not such a form. */
static uint8_t *form_file(uint8_t *p, size_t n, const char *ctype, size_t *len)
{
    char delim[80]; const char *b = strstr(ctype, "boundary="); size_t k, dl;
    const uint8_t *at, *end = p + n, *best = 0; size_t best_len = 0; int named = 0;
    if (!b) return 0;
    b += 9;
    if (*b == '"') { b++; k = strcspn(b, "\""); } else k = strcspn(b, "; \t");
    if (k == 0 || k > 70) return 0;
    snprintf(delim, sizeof delim, "\r\n--%.*s", (int)k, b);
    dl = k + 4;
    /* the first delimiter may come without the line end in front */
    at = n >= dl - 2 && !memcmp(p, delim + 2, dl - 2) ? p + dl - 2 : find(p, n, delim, dl);
    if (at && at != p + dl - 2) at += dl;
    while (at && at + 2 <= end && !(at[0] == '-' && at[1] == '-')) {
        const uint8_t *hend = find(at, (size_t)(end - at), "\r\n\r\n", 4), *next;
        int is_image;
        if (!hend) return 0;
        next = find(hend + 4, (size_t)(end - hend - 4), delim, dl);
        if (!next) return 0;
        is_image = find(at, (size_t)(hend - at), "name=\"image\"", 12) != 0;
        if ((is_image && !named) || !best) { best = hend + 4; best_len = (size_t)(next - hend - 4); named = is_image; }
        at = next + dl;
    }
    if (!best) return 0;
    *len = best_len;
    return p + (best - p);
}

#endif
