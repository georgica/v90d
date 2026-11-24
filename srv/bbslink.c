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
#include "bbssig.h"
#include "svc.h"

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
typedef int (*scan_fn)(void *ctx, int kind, const bbs_entry *e);   /* 1 = enough */
typedef int (*scan_raw_fn)(void *ctx, const char *line, size_t n);   /* the same, for a line as it is */
typedef struct { int fd; char line[BBS_LINE_MAX + 2]; size_t n; int skip; uint64_t serial; scan_raw_fn raw; } scan_t;

static int scan_open(scan_t *s, const char *path)
{
    s->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    s->n = 0; s->skip = 0; s->serial = 0; s->raw = NULL;
    return s->fd;
}
static void scan_close(scan_t *s) { if (s->fd >= 0) close(s->fd); s->fd = -1; }
static int scan_line(scan_t *s, scan_fn fn, void *ctx)
{
    bbs_entry e; int kind;
    if (s->raw) {
        if (!s->skip && !s->serial) s->serial = bbs_serial(s->line, s->n);
        kind = s->raw(ctx, s->skip ? NULL : s->line, s->n);
        s->n = 0; s->skip = 0;
        return kind;
    }
    kind = s->skip ? BBS_LINE_BAD : bbs_line(s->line, s->n, &e);
    if (kind == BBS_LINE_NONE && !s->serial) s->serial = bbs_serial(s->line, s->n);   /* "# serial N" (bbssig.h) */
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
/* ---- the lists from Netstalgia, kept up to date: the boards, and the
 * numbers of services. Each is signed (bbssig.h); the image has one of
 * each (builtin), a fetched one is kept in live once checked. ---- */
typedef struct lister lister_t;
struct lister {
    const char *tag;                                    /* for the log: "bbs", "svc" */
    const char *builtin, *live, *cmd;                   /* cmd "" = never fetch */
    int need_entries;                                   /* a list without entries is no list */
    int (*line)(lister_t *L, const char *line, size_t n);   /* a line of a list being checked: an LK_ */
    void (*taken)(lister_t *L);                         /* a list is in force (the one just checked) */
    int have_live; uint64_t live_serial;                /* the list in force is the fetched one */
    scan_t sc; int busy, builtin_scan, builtin_done;    /* a list is being checked: fetched, or the image's */
    int n_ok, n_bad, head, first;
    pid_t pid; uint32_t next_ms; int wanted, started, fails;
    char tmp[200];
};
enum { LK_NONE = 0, LK_HEAD, LK_OK, LK_BAD };

static int bbs_check_line(lister_t *L, const char *line, size_t n)
{
    bbs_entry e; int kind;
    (void)L;
    kind = line ? bbs_line(line, n, &e) : BBS_LINE_BAD;
    return kind == BBS_LINE_NONE ? LK_NONE : kind == BBS_LINE_HEAD ? LK_HEAD : kind == BBS_LINE_BBS ? LK_OK : LK_BAD;
}

/* Netstalgia's numbers of services: the table in force and the one being filled */
static cfg_service_t svc_tab[2][SVC_LIST_MAX];
static int svc_n[2], svc_cur;
static int svc_check_line(lister_t *L, const char *line, size_t n)
{
    cfg_service_t e; int kind, nx = !svc_cur, i;
    (void)L;
    kind = line ? svc_line(line, n, &e) : SVC_LINE_BAD;
    if (kind == SVC_LINE_HEAD) return LK_HEAD;
    if (kind == SVC_LINE_NONE || kind == SVC_LINE_OTHER) return LK_NONE;
    if (kind != SVC_LINE_SVC) return LK_BAD;
    for (i = 0; i < svc_n[nx]; i++) if (cfg_svc_number_match(svc_tab[nx][i].number, e.number)) return LK_BAD;   /* the first one counts */
    if (svc_n[nx] >= SVC_LIST_MAX) return LK_BAD;
    svc_tab[nx][svc_n[nx]++] = e;
    return LK_OK;
}
static void svc_taken(lister_t *L) { (void)L; svc_cur = !svc_cur; }

static lister_t bbs_list = { "bbs", "/etc/v90d.bbs", "/tmp/v90d.bbs", "sh /etc/bbsfetch.sh", 1, bbs_check_line, NULL,
                             .sc = { .fd = -1 } };
static lister_t svc_list = { "svc", "/etc/v90d.svc", "/tmp/v90d.svc",
                             "sh /etc/bbsfetch.sh http://api.netstalgia.online/v1/services.txt 'v90d-svc 1'",
                             0, svc_check_line, svc_taken, .sc = { .fd = -1 } };

/* the list of boards in force: the fetched one once there is one */
static const char *list_path(void) { return bbs_list.have_live ? bbs_list.live : bbs_list.builtin; }

/* The fetch, in a child of the daemon: the command writes the list to
 * standard output (the new file), then the signature on it is checked here,
 * away from the loop (a check takes a good part of a second on the box). */
#define FETCH_UNSIGNED 3        /* nobody we know signed it */
#define FETCH_OLD      4        /* signed, but older than the list in force (or no serial) */
#define FETCH_SAME     5        /* the list in force again */
static int fetch_child(lister_t *L)
{
    pid_t p; int st, r; uint64_t got, have;
    p = fork();
    if (p == 0) { execl("/bin/sh", "sh", "-c", L->cmd, (char *)NULL); _exit(127); }
    if (p < 0) return 126;
    while (waitpid(p, &st, 0) < 0) if (errno != EINTR) return 126;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return 1;
    r = bbs_sig_file(L->tmp, bbs_sig_keys, BBS_SIG_KEYS, &got);
    have = L->have_live ? L->live_serial : bbs_serial_file(L->builtin);
    switch (bbs_take(r, got, have)) {
    case BBS_TAKE:      return 0;
    case BBS_TAKE_SAME: return FETCH_SAME;
    case BBS_TAKE_OLD:  return FETCH_OLD;
    default:            return FETCH_UNSIGNED;
    }
}

static int lf_line(void *ctx, const char *line, size_t n)
{
    lister_t *L = ctx; int kind = L->line(L, line, n);
    if (kind == LK_NONE) return 0;
    if (L->first) { L->first = 0; L->head = kind == LK_HEAD; return !L->head; }   /* not a list at all */
    if (kind == LK_OK) L->n_ok++; else L->n_bad++;
    return 0;
}
static void lf_check(lister_t *L, const char *path, int builtin)
{
    if (L == &svc_list) svc_n[!svc_cur] = 0;
    if (scan_open(&L->sc, path) < 0) { if (builtin) L->builtin_done = 1; return; }
    L->sc.raw = lf_line;
    L->busy = 1; L->builtin_scan = builtin; L->n_ok = L->n_bad = 0; L->head = 0; L->first = 1;
}
static void lf_poll(lister_t *L, uint32_t now_ms)
{
    int st;
    if (L->busy) {
        int k;
        for (k = 0; k < 4 && L->busy; k++) if (scan_step(&L->sc, NULL, L)) L->busy = 0;
        if (L->busy) return;
        if (L->builtin_scan) {
            /* the image's list: no signature looked at, nothing renamed */
            L->builtin_done = 1;
            if (L->head && L->taken) L->taken(L);
            return;
        }
        if (!L->head || (L->need_entries && L->n_ok == 0)) {
            logf_(CFG_LOG_WARN, "%s: the list that came is not one; keeping the one there is", L->tag);
            (void)unlink(L->tmp);
            return;
        }
        if (rename(L->tmp, L->live) < 0) { logf_(CFG_LOG_WARN, "%s: the list that came cannot be kept: %s", L->tag, strerror(errno)); return; }
        L->have_live = 1; L->live_serial = L->sc.serial;
        if (L->taken) L->taken(L);
        logf_(CFG_LOG_INFO, "%s: fetched list with %d %s%s", L->tag, L->n_ok, L == &bbs_list ? "boards" : "numbers",
              L->n_bad ? " (some lines not understood)" : "");
        return;
    }
    if (L->taken && !L->builtin_done) { lf_check(L, L->builtin, 1); return; }    /* the image's list, first */
    if (L->pid > 0) {
        if (waitpid(L->pid, &st, WNOHANG) != L->pid) return;
        L->pid = 0;
        if (WIFEXITED(st) && (WEXITSTATUS(st) == FETCH_UNSIGNED || WEXITSTATUS(st) == FETCH_OLD || WEXITSTATUS(st) == FETCH_SAME)) {
            /* fetched, but not to be taken: nothing to try again soon */
            if (WEXITSTATUS(st) == FETCH_UNSIGNED) logf_(CFG_LOG_WARN, "%s: the list that came is not signed by Netstalgia; keeping the one there is", L->tag);
            else if (WEXITSTATUS(st) == FETCH_OLD) logf_(CFG_LOG_WARN, "%s: the list that came is older than the one there is; keeping that", L->tag);
            (void)unlink(L->tmp);
            L->next_ms = now_ms + 24u * 3600u * 1000u; L->fails = 0;
            return;
        }
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            L->next_ms = now_ms + 24u * 3600u * 1000u; L->fails = 0;
            lf_check(L, L->tmp, 0);
            if (L->busy) return;
        }
        {
            /* a box that has just started has no clock and perhaps no network yet: soon again, a few times */
            int soon = L->fails++ < 5;
            logf_(CFG_LOG_WARN, "%s: the list could not be fetched; trying again in %s", L->tag, soon ? "1 minute" : "15 minutes");
            (void)unlink(L->tmp);
            L->next_ms = now_ms + (soon ? 60000u : 900000u);
        }
        return;
    }
    if (!L->cmd[0] || !L->wanted) return;
    if (L->started && (int32_t)(now_ms - L->next_ms) < 0) return;
    L->started = 1; L->next_ms = now_ms + 900000u;
    if (snprintf(L->tmp, sizeof L->tmp, "%s.new", L->live) >= (int)sizeof L->tmp) return;
    L->pid = fork();
    if (L->pid == 0) {
        int fd;
        (void)unlink(L->tmp);                           /* a new file of our own, never through a link */
        fd = open(L->tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) _exit(126);
        child_setup(-1, fd);
        _exit(fetch_child(L));
    }
    if (L->pid < 0) L->pid = 0;
}

/* Netstalgia's entry for a number dialled, or NULL */
static const cfg_service_t *svc_find(const char *dialed)
{
    int i;
    for (i = 0; i < svc_n[svc_cur]; i++) if (cfg_svc_number_match(svc_tab[svc_cur][i].number, dialed)) return &svc_tab[svc_cur][i];
    return NULL;
}
int bbslink_svc_known(const char *dialed)
{
    int i, best = 0, m;
    for (i = 0; i < svc_n[svc_cur] && best < 2; i++)
        if ((m = cfg_svc_number_begins(svc_tab[svc_cur][i].number, dialed)) > best) best = m;
    return best;
}
void bbslink_svc_want(int on) { svc_list.wanted = on != 0; }

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
    int        svc, raw, policy;                    /* a number of a service; its mode raw; bbsdial_run's self_ok */
    uint8_t    dn[4096]; size_t dn_n;               /* for the modem, as it takes them */
    uint8_t    up[4096]; size_t up_n;               /* for the board (raw: the caller's first octets wait here) */
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
/* the service of the number dialled on port k into l->to: the owner's
 * first, then Netstalgia's. 0 if it is none (the list changed meanwhile). */
static int svc_entry(int k, struct bbs_link *l)
{
    const char *d = srv_dialed(&S[k]);
    const cfg_t *c = &S[k].cfg;
    const cfg_service_t *e = NULL;
    int i, whose = srv_svc_number(&S[k], d);
    if (whose == SRV_SVC_OWN)
        for (i = 0; i < c->n_services && i < CFG_MAX_SERVICES && !e; i++)
            if (cfg_svc_number_match(c->services[i].number, d)) e = &c->services[i];
    if (whose == SRV_SVC_LIST) e = svc_find(d);
    if (!e) return 0;
    memset(&l->to, 0, sizeof l->to);
    snprintf(l->to.host, sizeof l->to.host, "%s", e->host);
    snprintf(l->to.name, sizeof l->to.name, "%s", e->name);
    snprintf(l->to.number, sizeof l->to.number, "%.*s", (int)(sizeof l->to.number - 1), e->number);
    l->to.port = e->port;
    l->raw = e->mode == CFG_SVC_RAW;
    l->policy = whose == SRV_SVC_LIST ? BBSDIAL_PUBLIC : 0;    /* the owner's may be on the LAN */
    return 1;
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
    if (!l->raw) say(l, line);
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sv) < 0) { bl_bye(l, "The box cannot make the connection.\r\n"); return; }
    l->pid = fork();
    if (l->pid == 0) {
        (void)fcntl(sv[1], F_SETFL, 0);
        child_setup(sv[1], sv[1]);
        if (dial_prog[0]) execl(dial_prog, dial_prog, l->to.host, port, (char *)NULL);
        _exit(bbsdial_run(l->to.host, port, 0, 15, l->policy));    /* child_setup made the pair our fd 0 */
    }
    close(sv[1]);
    if (l->pid < 0) { l->pid = 0; close(sv[0]); bl_bye(l, "The box cannot make the connection.\r\n"); return; }
    l->fd = sv[0]; l->got = 0;
    if (!l->raw) l->up_n = 0;                           /* raw: what the caller sent so far goes first */
    telnet_init(&l->tn);
    l->st = BL_LINKED;
    logf_(CFG_LOG_INFO, "port %d: %s: calling %s:%s%s", k + 1, l->svc ? "svc" : "bbs", l->to.host, port,
          l->svc ? (l->raw ? " (raw)" : " (telnet)") : "");
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
    if (l->raw && (l->st == BL_RINGING || l->st == BL_LINKED)) {
        size_t take = n < sizeof l->up - l->up_n ? n : sizeof l->up - l->up_n;   /* what does not fit is lost */
        memcpy(l->up + l->up_n, oct, take); l->up_n += take;
        return;
    }
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
    lf_poll(&bbs_list, now_ms);
    lf_poll(&svc_list, now_ms);
    for (k = 0; k < n_ports; k++) {
        struct bbs_link *l = &bl[k]; uint32_t addr = 0; int on = 0, kind = srv_link(&S[k], &addr, &on);
        if (kind != SRV_LINK_BBS && kind != SRV_LINK_SVC) { if (l->st != BL_IDLE) { bl_close(l); l->st = BL_IDLE; } continue; }
        l->now = now_ms;
        switch (l->st) {
        case BL_IDLE:
            l->dn_n = 0; l->up_n = 0;
            l->st = BL_RINGING; l->t0 = now_ms;
            l->svc = kind == SRV_LINK_SVC; l->raw = 0; l->policy = 0;
            if (l->svc && !svc_entry(k, l)) { bl_bye(l, "That number cannot be called.\r\n"); break; }
            if (kind == SRV_LINK_BBS) bbs_list.wanted = 1;
            break;
        case BL_RINGING:
            if (!on) { l->t0 = now_ms; break; }
            if (l->raw) { bl_dial(k); break; }          /* a program's own protocol: at once, nothing said */
            if (now_ms - l->t0 < BL_GREET_MS) break;
            if (l->svc) { bl_dial(k); break; }          /* telnet: "Calling NAME ...", no prompt */
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
            uint8_t in[512], rep[64]; size_t out_n = 0, rep_n = 0; ssize_t r; int over = 0;
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
                if (l->raw) { memcpy(l->dn + l->dn_n, in, (size_t)r); l->dn_n += (size_t)r; continue; }
                (void)telnet_rx(&l->tn, in, (size_t)r, l->dn + l->dn_n, sizeof l->dn - l->dn_n, &out_n, rep, sizeof rep, &rep_n);
                l->dn_n += out_n;
                memcpy(l->up + l->up_n, rep, rep_n); l->up_n += rep_n;
            }
            if (!over) break;
            bl_close(l);
            if (l->got) { logf_(CFG_LOG_INFO, "port %d: %s: the %s hung up", k + 1, l->svc ? "svc" : "bbs", l->svc ? "service" : "board"); bl_bye(l, ""); break; }
            logf_(CFG_LOG_INFO, "port %d: %s: no answer from %s", k + 1, l->svc ? "svc" : "bbs", l->to.host);
            if (l->svc) { bl_bye(l, l->raw ? "" : "No answer.\r\n"); break; }     /* nothing else to call */
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
    bbs_list.wanted = 1;
    if (scan_open(&sc, list_path()) >= 0) while (!scan_step(&sc, row_line, &r)) { }
    *shown = r.shown; *left = r.left;
    return r.n;
}

/* ---- the Services page: Netstalgia's numbers as rows ---- */
size_t bbslink_svc_rows(char *buf, size_t cap, int *shown, int *left)
{
    struct rows r = { buf, cap, 0, 0, 0, 0, 0, "" };
    int i;
    *shown = 0; *left = 0;
    for (i = 0; i < svc_n[svc_cur]; i++) {
        const cfg_service_t *e = &svc_tab[svc_cur][i];
        char port[8];
        if (r.cap - r.n < 80 + 6 * (CFG_SVC_NAME_MAX + CFG_SVC_HOST_MAX + CFG_SVC_NUMBER_MAX)) { (*left)++; continue; }
#define PUT(lit) do { memcpy(r.buf + r.n, lit, sizeof(lit) - 1); r.n += sizeof(lit) - 1; } while (0)
        PUT("<tr><td>"); r.n += esc(r.buf + r.n, 6 * CFG_SVC_NUMBER_MAX, e->number);
        PUT("</td><td>"); r.n += esc(r.buf + r.n, 6 * CFG_SVC_NAME_MAX, e->name);
        PUT("</td><td>"); r.n += esc(r.buf + r.n, 6, e->mode == CFG_SVC_TELNET ? "telnet" : "raw");
        PUT("</td><td>"); r.n += esc(r.buf + r.n, 6 * CFG_SVC_HOST_MAX, e->host);
        snprintf(port, sizeof port, ":%u", (unsigned)e->port);
        r.n += esc(r.buf + r.n, sizeof port, port);
        PUT("</td></tr>\n");
#undef PUT
        (*shown)++;
    }
    return r.n;
}

void bbslink_stop(void)
{
    int k;
    for (k = 0; k < PORTS; k++) bl_close(&bl[k]);
    if (bbs_list.pid > 0) { (void)kill(bbs_list.pid, SIGKILL); (void)waitpid(bbs_list.pid, NULL, 0); bbs_list.pid = 0; }
    if (svc_list.pid > 0) { (void)kill(svc_list.pid, SIGKILL); (void)waitpid(svc_list.pid, NULL, 0); svc_list.pid = 0; }
}

void bbslink_set_builtin(const char *path) { bbs_list.builtin = path; }
void bbslink_set_live(const char *path)    { bbs_list.live = path; }
void bbslink_set_fetch(const char *cmd)    { bbs_list.cmd = cmd ? cmd : ""; }
void bbslink_svc_set_builtin(const char *path) { svc_list.builtin = path; }
void bbslink_svc_set_live(const char *path)    { svc_list.live = path; }
void bbslink_svc_set_fetch(const char *cmd)    { svc_list.cmd = cmd ? cmd : ""; }
void bbslink_set_prog(const char *prog)    { dial_prog = prog ? prog : ""; }
