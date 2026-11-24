/* See update.h. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "daemon.h"
#include "update.h"

static const char *check_cmd  = "sh /etc/updatecheck.sh";
static const char *state_path = "/tmp/v90d.update";
static int   enabled;
static char  running[24] = "";          /* the version this box runs, "" until told */

/* ---- version compare ---------------------------------------------------- */

/* read up to four dot-separated components, a leading 'v' allowed; a
 * component that is not there counts as 0, so "0.5" < "0.5.1". */
static void ver_parse(const char *s, long v[4])
{
    int i = 0;
    v[0] = v[1] = v[2] = v[3] = 0;
    if (!s) return;
    if (*s == 'v' || *s == 'V') s++;
    while (*s && i < 4) {
        long x = 0; int any = 0;
        while (*s >= '0' && *s <= '9') { x = x * 10 + (*s - '0'); any = 1; s++; if (x > 1000000000L) x = 1000000000L; }
        v[i++] = any ? x : 0;
        if (*s == '.') s++; else break;
    }
}

int update_ver_cmp(const char *a, const char *b)
{
    long va[4], vb[4]; int i;
    ver_parse(a, va); ver_parse(b, vb);
    for (i = 0; i < 4; i++) if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

/* a plausible version string: 1..23 of digits and dots, at least one digit */
static int ver_ok(const char *s)
{
    int i, dig = 0;
    for (i = 0; s[i]; i++) {
        if (i >= 23) return 0;
        if (s[i] >= '0' && s[i] <= '9') dig = 1;
        else if (s[i] != '.') return 0;
    }
    return i > 0 && dig;
}

/* ---- the child and its result ------------------------------------------- */

/* pull "version=X.Y.Z" out of the child's output into out[0..cap) */
static int read_version(const char *path, char *out, size_t cap)
{
    char buf[512]; int fd; ssize_t n; char *p, *q;
    out[0] = '\0';
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    p = strstr(buf, "version=");
    if (!p || (p != buf && p[-1] != '\n')) return 0;
    p += 8;
    for (q = p; *q && *q != '\n' && *q != '\r' && (size_t)(q - p) < cap - 1; q++) out[q - p] = *q;
    out[q - p] = '\0';
    return out[0] != '\0';
}

/* the child finished well: decide whether the latest is newer than us and
 * leave the Firmware page the answer (the file is absent when up to date) */
static void consider(const char *tmp)
{
    char latest[32]; int fd;
    if (!read_version(tmp, latest, sizeof latest) || !ver_ok(latest)) {
        logf_(CFG_LOG_WARN, "update: the reply from GitHub was not understood");
        return;
    }
    if (running[0] && update_ver_cmp(latest, running) > 0) {
        char line[64];
        int ln = snprintf(line, sizeof line, "version=%s\n", latest);
        (void)unlink(state_path);
        if (ln > 0 && (fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600)) >= 0) {
            (void)!write(fd, line, (size_t)ln);       /* a note only: nothing to do if it fails */
            close(fd);
        }
        logf_(CFG_LOG_INFO, "update: %s is available (this box runs %s)", latest, running);
    } else {
        (void)unlink(state_path);          /* up to date: nothing to offer */
    }
}

int update_available(char *ver, size_t cap)
{
    if (cap) ver[0] = '\0';
    if (!read_version(state_path, ver, cap)) return 0;
    if (!ver_ok(ver)) { if (cap) ver[0] = '\0'; return 0; }
    return 1;
}

static struct {
    pid_t pid; uint32_t next_ms; int started, fails;
    char tmp[64];
} ud;

void update_poll(uint32_t now_ms)
{
    int st;
    if (ud.pid > 0) {                      /* a check is running */
        if (waitpid(ud.pid, &st, WNOHANG) != ud.pid) return;
        ud.pid = 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            ud.next_ms = now_ms + 24u * 3600u * 1000u; ud.fails = 0;
            consider(ud.tmp);
        } else {
            int soon = ud.fails++ < 5;     /* a box that just started may have no clock or network yet */
            logf_(CFG_LOG_WARN, "update: could not reach GitHub; trying again in %s", soon ? "1 minute" : "15 minutes");
            ud.next_ms = now_ms + (soon ? 60000u : 900000u);
        }
        (void)unlink(ud.tmp);
        return;
    }
    if (!enabled || !check_cmd[0] || !running[0]) return;
    if (ud.started && (int32_t)(now_ms - ud.next_ms) < 0) return;
    ud.started = 1; ud.next_ms = now_ms + 900000u;
    if (snprintf(ud.tmp, sizeof ud.tmp, "%s.new", state_path) >= (int)sizeof ud.tmp) return;
    ud.pid = fork();
    if (ud.pid == 0) {
        int fd;
        (void)unlink(ud.tmp);              /* a file of our own, never through a link */
        fd = open(ud.tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) _exit(126);
        child_plain(-1, fd, 1);
        execl("/bin/sh", "sh", "-c", check_cmd, (char *)NULL);
        _exit(127);
    }
    if (ud.pid < 0) ud.pid = 0;
}

void update_config(int on, const char *running_version)
{
    int was = enabled;
    enabled = on != 0;
    running[0] = '\0';
    if (running_version) { strncpy(running, running_version, sizeof running - 1); running[sizeof running - 1] = '\0'; }
    if (!enabled) {
        (void)unlink(state_path);          /* turned off: stop offering, stop phoning home */
        ud.started = 0;
    } else if (!was) {
        ud.started = 0;                    /* check soon after being turned on (not on every save) */
    }
}

void update_stop(void)
{
    if (ud.pid > 0) { (void)kill(ud.pid, SIGKILL); (void)waitpid(ud.pid, NULL, 0); ud.pid = 0; }
    (void)unlink(ud.tmp);
}

void update_set_cmd(const char *cmd)     { check_cmd = cmd ? cmd : ""; }
void update_set_state(const char *path)  { if (path) state_path = path; }
