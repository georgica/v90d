/* See nv.h. The two actions of the Firmware page that write the flash,
 * done here and not in the daemon: the daemon answers telephone lines
 * every 10 ms and must not wait for a megabyte from a browser or for a
 * flash chip. It checks who is asking, then starts us with the browser's
 * connection as our standard input and output and forgets about it.
 *
 *   v90nv serve-upgrade LEN [CONTENT-TYPE]   the body is the new firmware:
 *       a form upload (multipart/form-data, the field is called "image") or
 *       the file as it is
 *   v90nv serve-stock LEN                    the body is the confirmation
 *       form (confirm=stock)
 *
 * What of the body the daemon had already read comes first, through the
 * descriptor named in V90NV_EARLY_FD. The answer is a whole HTTP response
 * with the pages' common look (cfg/page.h). For an upgrade it is sent when
 * the file has been checked and before the flash is touched; how the write
 * went is then kept in a small file the Firmware page shows
 * (V90NV_STATE, /tmp/v90d.fwstate). When the flash is as asked a note is
 * left for the boot script, which restarts the box (V90NV_RESTART_NOTE,
 * /tmp/v90d.restart: the same note as the Restart button's). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "nv.h"
#include "sbuf.h"
#include "page.h"

/* page.h: the pages this tool makes (while the flash is written) show the
 * tabs every box has */
int page_consoles_tab;

#define BODY_MAX (NV_FILE_MAX + 4096u)  /* a file of the largest size inside a form */
#define RECV_S   20                     /* a browser that sends nothing for this long is gone */

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

static int serve_upgrade(const char *flash, long want, const char *ctype)
{
    static nvflash f; static nv_where w;
    const uint8_t *hdr, *img; size_t n, len; uint8_t *p, *file; char msg[200] = "", line[320]; int r;

    if (want <= 0) { answer(400, "not written", "No file came with the request.", "", 0); return 1; }
    if ((unsigned long)want > BODY_MAX) { answer(413, "not written", "The file is too large to be a firmware for this box.", "", 0); return 1; }
    if (!(p = malloc((size_t)want))) { answer(500, "not written", "Not enough memory to take the file.", "", 0); return 1; }
    if (read_body(p, (size_t)want) != want) { answer(400, "not written", "The upload broke off before the whole file had arrived.", "", 0); return 1; }
    file = p; n = (size_t)want;
    if (ctype && !strncmp(ctype, "multipart/form-data", 19)) {
        file = form_file(p, (size_t)want, ctype, &n);
        if (!file) { answer(400, "not written", "The upload is not a form this page understands.", "", 0); return 1; }
    }
    if (n == 0) { answer(400, "not written", "No file was chosen.", "", 0); return 1; }
    if (n > NV_FILE_MAX) { answer(413, "not written", "The file is too large to be a firmware for this box.", "", 0); return 1; }
    if (nf_open(&f, flash) != NV_OK || nv_locate(&f, &w, 0) != NV_OK) { answer(500, "not written", "The flash of the box cannot be read.", "", 0); return 2; }
    r = nv_upgrade_check(&f, &w, file, n, &hdr, &img, &len, msg, sizeof msg);
    if (r) { answer(400, "not written", "This file is not written to the box.", msg, 0); return 1; }

    snprintf(line, sizeof line, "The file is good: %s. It is being written to the flash now.", msg);
    answer(200, "writing", line,
           "This takes a few minutes; then the box restarts by itself with the new firmware. Leave the power on. "
           "If the power does go while it writes, the box starts the stock firmware, and the v90d firmware file has to be "
           "given to that again. This page goes back to the Firmware page by itself, which shows how the writing went "
           "for as long as the old system still answers.", 180);
    hang_up();
    snprintf(line, sizeof line, "writing %s", msg);
    note_state(line);
    r = nv_upgrade_write(&f, &w, hdr, img, len, msg, sizeof msg);
    if (r) {
        snprintf(line, sizeof line, "FAILED (%d): the new firmware is not in the flash, and the old one may be gone from it too. "
                 "The system that runs now is in memory and still whole: upload the file again before the box is restarted.", r);
        note_state(line);
        return 2;
    }
    snprintf(line, sizeof line, "%s; the box restarts", msg);
    note_state(line);
    if (ask_restart()) note_state("written, but the restart could not be asked for: use the Restart button");
    return 0;
}

static int serve_stock(const char *flash, long want)
{
    static nvflash f; static nv_where w;
    char body[64], msg[200] = ""; int r;
    if (want < 0 || want >= (long)sizeof body) { answer(400, "not done", "This is not the confirmation form.", "", 0); return 1; }
    if (read_body((uint8_t *)body, (size_t)want) != want) return 1;
    body[want] = 0;
    if (strcmp(body, "confirm=stock")) { answer(400, "not done", "Nothing was changed: the box was not ticked.", "", 0); return 1; }
    if (nf_open(&f, flash) != NV_OK || nv_locate(&f, &w, 1) != NV_OK) { answer(500, "not done", "The flash of the box cannot be read.", "", 0); return 2; }
    r = nv_to_stock(&f, &w, msg, sizeof msg);
    if (r > 0) { answer(409, "not done", "The box stays on this firmware.", msg, 0); return 1; }
    if (r < 0) { answer(500, "not done", "The flash did not take the change. The box stays as it is until it is restarted.", "", 0); return 2; }
    answer(200, "restarting", "The box is restarting into the stock firmware.",
           "It will answer at the address the stock firmware is set up for, with the stock firmware's own pages and password. "
           "To come back, give the v90d firmware file to its upgrade page.", 0);
    hang_up();
    note_state("back to the stock firmware; the box restarts");
    if (ask_restart()) return 2;
    return 0;
}

int nv_serve(const char *what, const char *flash, int argc, char **argv)
{
    long want;
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return 2;
    want = strtol(argv[1], 0, 10);
    if (!strcmp(what, "serve-upgrade") && argc <= 3) return serve_upgrade(flash, want, argc == 3 ? argv[2] : 0);
    if (!strcmp(what, "serve-stock") && argc == 2) return serve_stock(flash, want);
    return 2;
}
