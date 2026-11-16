/* See bbslink.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "daemon.h"
#include "bbslink.h"
#include "bbs.h"
#include "telnet.h"
#include "bbsdial.h"

static const char *builtin_path = "/etc/v90d.bbs";
static const char *live_path = "/tmp/v90d.bbs";
static const char *fetch_cmd = "sh /etc/bbsfetch.sh";     /* from the Telnet BBS Guide: fw/bbsfetch.sh */
static const char *dial_prog = "";

/* a child of the real-time daemon is an ordinary process with nothing of
 * ours open but what it is given as 0 and 1 */
static void child_setup(int in, int out)
{
    struct sched_param sp; int fd;
    memset(&sp, 0, sizeof sp);
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);
    (void)munlockall();
    if (in >= 0 && dup2(in, 0) < 0) _exit(126);
    if (out >= 0 && dup2(out, 1) < 0) _exit(126);
    for (fd = 3; fd < 1024; fd++) (void)close(fd);
}

/* ---- the list, a line at a time: a file is never read in one go, the
 * loop has a telephone line to serve ---- */
#define SCAN_CHUNK 512
typedef struct { int fd; char line[BBS_LINE_MAX + 2]; size_t n; int skip; } scan_t;
typedef int (*scan_fn)(void *ctx, int kind, const bbs_entry *e);   /* 1 = enough */

static int scan_open(scan_t *s, const char *path)
{
    s->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    s->n = 0; s->skip = 0;
    return s->fd;
}
static void scan_close(scan_t *s) { if (s->fd >= 0) close(s->fd); s->fd = -1; }
static int scan_line(scan_t *s, scan_fn fn, void *ctx)
{
    bbs_entry e; int kind = s->skip ? BBS_LINE_BAD : bbs_line(s->line, s->n, &e);
    s->n = 0; s->skip = 0;
    return fn(ctx, kind, &e);
}
/* the next piece of the file. 0 = more to come, 1 = the end (or fn had
 * enough): the file is closed */
static int scan_step(scan_t *s, scan_fn fn, void *ctx)
{
    char buf[SCAN_CHUNK]; ssize_t r, i;
    r = read(s->fd, buf, sizeof buf);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (r <= 0) {
        if (s->n || s->skip) (void)scan_line(s, fn, ctx);
        scan_close(s);
        return 1;
    }
    for (i = 0; i < r; i++) {
        if (buf[i] == '\n') { if (scan_line(s, fn, ctx)) { scan_close(s); return 1; } }
        else if (s->n < BBS_LINE_MAX + 1) s->line[s->n++] = buf[i];
        else s->skip = 1;                               /* longer than a line can be */
    }
    return 0;
}
/* the list in force: the fetched one once there is one */
static int have_live;
static const char *list_path(void) { return have_live ? live_path : builtin_path; }


/* ---- keeping the list up to date. Nothing is fetched by a box whose
 * owner never looks at the boards: the first call to the number or the
 * first look at the BBS page asks for it, then once a day. ---- */
static struct {
    scan_t sc; int busy;                                /* a fetched list is being checked */
    int n_bbs, n_bad, head, first;
    pid_t pid; uint32_t next_ms; int wanted, started, fails;
    char tmp[200];
} ld = { .sc = { .fd = -1 } };

static int ld_line(void *ctx, int kind, const bbs_entry *e)
{
    (void)ctx; (void)e;
    if (kind == BBS_LINE_NONE) return 0;
    if (ld.first) { ld.first = 0; ld.head = kind == BBS_LINE_HEAD; return !ld.head; }   /* not a list at all */
    if (kind == BBS_LINE_BBS) ld.n_bbs++; else ld.n_bad++;
    return 0;
}
static void ld_poll(uint32_t now_ms)
{
    int st;
    if (ld.busy) {
        int k;
        for (k = 0; k < 4 && ld.busy; k++) if (scan_step(&ld.sc, ld_line, NULL)) ld.busy = 0;
        if (ld.busy) return;
        if (!ld.head || ld.n_bbs == 0) {
            logf_(CFG_LOG_WARN, "bbs: the list that came is not one; keeping the one there is");
            (void)unlink(ld.tmp);
            return;
        }
        if (rename(ld.tmp, live_path) < 0) { logf_(CFG_LOG_WARN, "bbs: the list that came cannot be kept: %s", strerror(errno)); return; }
        have_live = 1;
        logf_(CFG_LOG_INFO, "bbs: fetched list with %d boards%s", ld.n_bbs, ld.n_bad ? " (some lines not understood)" : "");
        return;
    }
    if (ld.pid > 0) {
        if (waitpid(ld.pid, &st, WNOHANG) != ld.pid) return;
        ld.pid = 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && scan_open(&ld.sc, ld.tmp) >= 0) {
            ld.next_ms = now_ms + 24u * 3600u * 1000u; ld.fails = 0;
            ld.busy = 1; ld.n_bbs = ld.n_bad = 0; ld.head = 0; ld.first = 1;
        } else {
            /* a box that has just started has no clock and perhaps no network yet: soon again, a few times */
            int soon = ld.fails++ < 5;
            logf_(CFG_LOG_WARN, "bbs: the list could not be fetched; trying again in %s", soon ? "1 minute" : "15 minutes");
            (void)unlink(ld.tmp);
            ld.next_ms = now_ms + (soon ? 60000u : 900000u);
        }
        return;
    }
    if (!fetch_cmd[0] || !ld.wanted) return;
    if (ld.started && (int32_t)(now_ms - ld.next_ms) < 0) return;
    ld.started = 1; ld.next_ms = now_ms + 900000u;
    if (snprintf(ld.tmp, sizeof ld.tmp, "%s.new", live_path) >= (int)sizeof ld.tmp) return;
    ld.pid = fork();
    if (ld.pid == 0) {
        int fd;
        (void)unlink(ld.tmp);                           /* a new file of our own, never through a link */
        fd = open(ld.tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) _exit(126);
        child_setup(-1, fd);
        execl("/bin/sh", "sh", "-c", fetch_cmd, (char *)NULL);
        _exit(127);
    }
    if (ld.pid < 0) ld.pid = 0;
}

/* ---- one caller ---- */
enum { BL_IDLE = 0, BL_RINGING, BL_PROMPT, BL_FIND, BL_LIST, BL_LINKED, BL_CLOSING, BL_OVER };
#define BL_IDLE_MS   180000u    /* at the prompt with nothing typed */
#define BL_LIST_MAX  40         /* boards shown for one search */
#define BL_GREET_MS  1500u      /* after the modems connect: a terminal program is not listening at once */
#define BL_LAST_MS   3000u      /* for the last words to reach the caller before the line is dropped */
static struct bbs_link {
    int        st, shown, more;
    uint32_t   t0, now;
    bbs_edit_t ed;
    scan_t     sc;
    char       key[BBS_LOGIN_MAX + 1];
    char       num[BBS_NUMBER_LEN + 1];             /* a board by its number, "" = by its name */
    bbs_entry  to; int found;
    telnet_t   tn;
    int        fd; pid_t pid; long got;
    uint8_t    dn[4096]; size_t dn_n;               /* for the modem, as it takes them */
    uint8_t    up[2048]; size_t up_n;               /* for the board */
} bl[PORTS] = { { .fd = -1, .sc = { .fd = -1 } }, { .fd = -1, .sc = { .fd = -1 } } };

static void say(struct bbs_link *l, const char *s)
{
    size_t n = strlen(s);
    if (n > sizeof l->dn - l->dn_n) n = sizeof l->dn - l->dn_n;
    memcpy(l->dn + l->dn_n, s, n); l->dn_n += n;
}
static void bl_close(struct bbs_link *l)
{
    if (l->pid > 0) { (void)kill(l->pid, SIGKILL); (void)waitpid(l->pid, NULL, 0); }
    if (l->fd >= 0) close(l->fd);
    scan_close(&l->sc);
    l->pid = 0; l->fd = -1;
}
static void bl_prompt(struct bbs_link *l)
{
    bbs_edit_init(&l->ed);
    say(l, "\r\nBBS: ");
    l->st = BL_PROMPT; l->t0 = l->now;
}
/* the last words, then the line is dropped */
static void bl_bye(struct bbs_link *l, const char *words)
{
    bl_close(l);
    say(l, words);
    l->st = BL_CLOSING; l->t0 = l->now;
}
#define HOST_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"
static void bl_dial(int k)
{
    struct bbs_link *l = &bl[k]; int sv[2]; char port[8], line[160];
    /* bbs.c let only letters, digits, '.' and '-' through, and no '-' in
     * front; what becomes an argument of a program is looked at once more */
    if (l->to.host[0] == '-' || !l->to.host[0] || strspn(l->to.host, HOST_CHARS) != strlen(l->to.host)) {
        bl_bye(l, "That board cannot be called.\r\n");
        return;
    }
    snprintf(port, sizeof port, "%u", (unsigned)l->to.port);
    snprintf(line, sizeof line, "Calling %s ...\r\n", l->to.name);
    say(l, line);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0) { bl_bye(l, "The box cannot make the connection.\r\n"); return; }
    l->pid = fork();
    if (l->pid == 0) {
        (void)fcntl(sv[1], F_SETFL, 0);
        child_setup(sv[1], sv[1]);
        if (dial_prog[0]) execl(dial_prog, dial_prog, l->to.host, port, (char *)NULL);
        _exit(bbsdial_run(l->to.host, port, 0, 15));    /* child_setup made the pair our fd 0 */
    }
    close(sv[1]);
    if (l->pid < 0) { l->pid = 0; close(sv[0]); bl_bye(l, "The box cannot make the connection.\r\n"); return; }
    l->fd = sv[0]; l->got = 0; l->up_n = 0;
    telnet_init(&l->tn);
    l->st = BL_LINKED;
    logf_(CFG_LOG_INFO, "port %d: bbs: calling %s:%s", k + 1, l->to.host, port);
}
static int find_line(void *ctx, int kind, const bbs_entry *e)
{
    struct bbs_link *l = ctx;
    if (kind != BBS_LINE_BBS) return 0;
    if (strcmp(e->login, l->key) != 0 && (!l->num[0] || strcmp(e->number, l->num) != 0)) return 0;
    l->to = *e; l->found = 1;
    return 1;
}
static int list_line(void *ctx, int kind, const bbs_entry *e)
{
    struct bbs_link *l = ctx; char line[140], num[BBS_LOGIN_MAX + 1];
    if (kind != BBS_LINE_BBS || !bbs_match(e, l->key)) return 0;
    if (l->shown == BL_LIST_MAX) { l->more = 1; return 1; }
    bbs_show(e->number, num);
    snprintf(line, sizeof line, "  %s  %-20s %s\r\n", num, e->login, e->name);
    say(l, line); l->shown++;
    return 0;
}
static void bl_find(struct bbs_link *l)
{
    l->found = 0;
    if (scan_open(&l->sc, list_path()) < 0) { l->st = BL_FIND; return; }    /* no list: found nothing, below */
    l->st = BL_FIND;
}
/* a line was typed at the prompt */
static void bl_typed(int k)
{
    struct bbs_link *l = &bl[k]; const char *t = l->ed.line;
    while (*t == ' ') t++;
    if (!*t) { bl_prompt(l); return; }
    if (*t == '?') {
        bbs_key(t + 1, l->key);
        l->shown = 0; l->more = 0;
        (void)scan_open(&l->sc, list_path());
        l->st = BL_LIST;
        return;
    }
    bbs_key(t, l->key);                                 /* a name, or a board's number */
    if (!l->key[0]) { bl_prompt(l); return; }
    if (bbs_number(l->key, l->num) != BBS_NUM_BOARD) l->num[0] = 0;
    bl_find(l);
}

void bbslink_from_modem(void *ctx, const uint8_t *oct, size_t n)
{
    int k = (int)(((srv_t *)ctx) - S); struct bbs_link *l = &bl[k]; size_t i;
    if (l->st == BL_LINKED) {
        size_t on = 0;
        (void)telnet_tx(&l->tn, oct, n, l->up + l->up_n, sizeof l->up - l->up_n, &on);   /* what does not fit is lost */
        l->up_n += on;
        return;
    }
    for (i = 0; i < n && l->st == BL_PROMPT; i++) {
        uint8_t echo[3]; size_t en;
        int done = bbs_edit(&l->ed, oct[i], echo, &en);
        l->t0 = l->now;
        if (en <= sizeof l->dn - l->dn_n) { memcpy(l->dn + l->dn_n, echo, en); l->dn_n += en; }
        if (done) bl_typed(k);
    }
}

void bbslink_poll(uint32_t now_ms)
{
    int k;
    ld_poll(now_ms);
    for (k = 0; k < n_ports; k++) {
        struct bbs_link *l = &bl[k]; uint32_t addr = 0; int on = 0, kind = srv_link(&S[k], &addr, &on);
        if (kind != SRV_LINK_BBS) { if (l->st != BL_IDLE) { bl_close(l); l->st = BL_IDLE; } continue; }
        l->now = now_ms;
        switch (l->st) {
        case BL_IDLE:
            l->dn_n = 0; l->up_n = 0;
            l->st = BL_RINGING; l->t0 = now_ms;
            ld.wanted = 1;
            break;
        case BL_RINGING:
            if (!on) { l->t0 = now_ms; break; }
            if (now_ms - l->t0 < BL_GREET_MS) break;
            say(l, "\r\nv90d bulletin board gateway\r\n");
            /* a board's own number was dialled: straight to it */
            if (bbs_number(srv_dialed(&S[k]), l->num) == BBS_NUM_BOARD) { l->key[0] = 0; bl_find(l); break; }
            l->num[0] = 0;
            say(l, "Type a board's name or number, or ? and part of a name to search.\r\n");
            bl_prompt(l);
            break;
        case BL_PROMPT:
            if (now_ms - l->t0 >= BL_IDLE_MS) bl_bye(l, "\r\nGoodbye.\r\n");
            break;
        case BL_FIND:
            if (l->sc.fd >= 0 && !scan_step(&l->sc, find_line, l)) break;
            if (l->found) { bl_dial(k); break; }
            say(l, l->num[0] ? "No board has that number. ? and part of a name searches.\r\n"
                             : "No board of that name. ? and part of a name searches.\r\n");
            bl_prompt(l);
            break;
        case BL_LIST:
            if (l->dn_n > sizeof l->dn / 2) break;      /* as fast as the caller reads */
            if (l->sc.fd >= 0 && !scan_step(&l->sc, list_line, l)) break;
            if (l->more) say(l, "  ... and more: type more of the name.\r\n");
            else if (!l->shown) say(l, "Nothing found.\r\n");
            bl_prompt(l);
            break;
        case BL_LINKED: {
            uint8_t in[512], rep[64]; size_t out_n, rep_n; ssize_t r; int over = 0;
            if (l->up_n) {
                r = send(l->fd, l->up, l->up_n, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (r > 0) { memmove(l->up, l->up + r, l->up_n - (size_t)r); l->up_n -= (size_t)r; }
                else if (r < 0 && errno != EAGAIN && errno != EINTR) over = 1;
            }
            /* read only what can be handed on whole: the board waits for a slow modem, nothing is dropped */
            while (!over && sizeof l->dn - l->dn_n >= sizeof in && sizeof l->up - l->up_n >= sizeof rep) {
                r = recv(l->fd, in, sizeof in, MSG_DONTWAIT);
                if (r < 0 && (errno == EAGAIN || errno == EINTR)) break;
                if (r <= 0) { over = 1; break; }
                l->got += r;
                (void)telnet_rx(&l->tn, in, (size_t)r, l->dn + l->dn_n, sizeof l->dn - l->dn_n, &out_n, rep, sizeof rep, &rep_n);
                l->dn_n += out_n;
                memcpy(l->up + l->up_n, rep, rep_n); l->up_n += rep_n;
            }
            if (!over) break;
            bl_close(l);
            if (l->got) { logf_(CFG_LOG_INFO, "port %d: bbs: the board hung up", k + 1); bl_bye(l, ""); break; }
            logf_(CFG_LOG_INFO, "port %d: bbs: no answer from %s", k + 1, l->to.host);
            say(l, "No answer.\r\n");
            bl_prompt(l);
            break; }
        case BL_CLOSING:
            if (l->dn_n == 0 && now_ms - l->t0 >= BL_LAST_MS) { l->st = BL_OVER; srv_hangup(&S[k]); }
            else if (l->dn_n) l->t0 = now_ms;
            break;
        default:        /* BL_OVER: until the call has ended */
            break;
        }
        if (l->dn_n) {
            size_t took = srv_send_raw(&S[k], l->dn, l->dn_n);
            if (took) { memmove(l->dn, l->dn + took, l->dn_n - took); l->dn_n -= took; }
        }
    }
}

/* ---- the BBS page: the boards as rows of a table ---- */
static size_t esc(char *out, size_t cap, const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        const char *r = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : NULL;
        size_t k = r ? strlen(r) : 1;
        if (n + k > cap) break;
        if (r) memcpy(out + n, r, k); else out[n] = *s;
        n += k;
    }
    return n;
}
struct rows { char *buf; size_t cap, n; int shown, left, skip, max; const char *q; };
static int row_line(void *ctx, int kind, const bbs_entry *e)
{
    struct rows *r = ctx; char num[BBS_LOGIN_MAX + 1]; size_t need = 80 + 6 * BBS_NAME_MAX + 2 * BBS_LOGIN_MAX;
    if (kind != BBS_LINE_BBS || !bbs_match(e, r->q)) return 0;
    if (r->skip > 0) { r->skip--; return 0; }
    if (r->shown >= r->max || r->cap - r->n < need) { r->left++; return 0; }
    bbs_show(e->number, num);
#define PUT(lit) do { memcpy(r->buf + r->n, lit, sizeof(lit) - 1); r->n += sizeof(lit) - 1; } while (0)
    PUT("<tr><td>"); r->n += esc(r->buf + r->n, 6 * BBS_NAME_MAX, e->name);
    PUT("</td><td><code>"); r->n += esc(r->buf + r->n, BBS_LOGIN_MAX, e->login);
    PUT("</code></td><td>"); r->n += esc(r->buf + r->n, BBS_LOGIN_MAX, num);
    PUT("</td></tr>\n");
#undef PUT
    r->shown++;
    return 0;
}
size_t bbslink_rows(void *ctx, const char *q, int skip, int max, char *buf, size_t cap, int *shown, int *left)
{
    static scan_t sc = { .fd = -1 }; struct rows r = { buf, cap, 0, 0, 0, skip, max, q };
    (void)ctx;
    ld.wanted = 1;
    if (scan_open(&sc, list_path()) >= 0) while (!scan_step(&sc, row_line, &r)) { }
    *shown = r.shown; *left = r.left;
    return r.n;
}

void bbslink_stop(void)
{
    int k;
    for (k = 0; k < PORTS; k++) bl_close(&bl[k]);
    if (ld.pid > 0) { (void)kill(ld.pid, SIGKILL); (void)waitpid(ld.pid, NULL, 0); ld.pid = 0; }
}

void bbslink_set_builtin(const char *path) { builtin_path = path; }
void bbslink_set_live(const char *path)    { live_path = path; }
void bbslink_set_fetch(const char *cmd)    { fetch_cmd = cmd ? cmd : ""; }
void bbslink_set_prog(const char *prog)    { dial_prog = prog ? prog : ""; }
