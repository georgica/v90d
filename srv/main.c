/* v90d - the dial-up server daemon (Linux).
 *
 *   v90d [-c FILE] -l LINE [-l LINE2] [-d N] [-i LAN-IF] [-t TUN] [-L LED-PREFIX] [-v]
 *   v90d [-c FILE] -p          print the LAN settings for the boot script
 *
 *   -c FILE    configuration file            [/etc/v90d.conf]
 *   -l LINE    telephone line, see line.h    (required unless -p). Given
 *              twice: the box's two ports; the second one runs on the
 *              [modem2] / [line2] settings and hands out ppp_peer2
 *   -d N       the line's echo bulk delay in samples, if the driver knows it
 *              (see line.h)                  [measured in every V.90 call]
 *   -i LAN-IF  LAN interface: the status and settings pages are served on
 *              its address only              [eth0]
 *   -t TUN     name of the PPP-side network device [ppp0]; the second
 *              port's is the same name with the next digit (ppp1)
 *   -v         log at debug level whatever the file says
 *
 * For the bench (none of these is a setting of the box; DESIGN.md "Linear
 * receive"):
 *   -R FORMATS receive format of the line's codec, one letter per port in
 *              the order of the -l options: l = 16-bit linear, c = companded
 *              (G.711, as the driver starts). "l" or "lc" or "ll" ... Only a
 *              dev: line can be linear; what we send is G.711 levels either
 *              way. Without -R a dev: line is asked for linear (measured
 *              on the unit: 2400 to 5300 bit/s more upstream) and stays
 *              companded if its driver cannot; other lines are left alone.
 *   -P CODE    the most downstream power a V.90 / V.92 caller may ask for,
 *              as INFO0d announces it: -0.5 dBm0 less 0.5 dB per step,
 *              1..31                         [23: -12 dBm0]
 *   -w FILE    record port 1's line while it is off hook, one call: per
 *              10 ms block the 80 codewords heard and the 80 sent. With
 *              linear receive the file begins with the 8 octets "V90DLIN1"
 *              and a block is 80 little-endian 16-bit samples heard and
 *              the 80 codewords sent (live/replay reads both kinds)
 *   -W FILE    the same for port 2
 *
 * One loop, clocked by the lines: every 10 ms each line delivers a block of
 * 80 samples, which goes through that port's server core (srv.h) and is
 * answered at once; between blocks IP packets move between the TUN devices
 * and the calls, and the HTTP server gets its turn. Nothing is allocated
 * after start-up and nothing blocks but the wait for the next block. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "srv.h"
#include "httpd.h"
#include "line.h"
#include "tun.h"
#include "nat.h"
#include "dclist.h"
#include "dcnow.h"

#define PORTS CFG_PORTS
static srv_t    S[PORTS];
static int      n_ports;
static httpd_t  H;
static cfg_t    live;               /* what the settings page shows and edits */
static const char *cfg_path = "/etc/v90d.conf";
#define RESTART_NOTE "/tmp/v90d.restart"
static const char *lan_if = "eth0";
static char     tun_name[PORTS][16] = { "ppp0", "ppp1" };
static int      tun_fd[PORTS] = { -1, -1 };
static cfg_t    unsaved; static int have_unsaved;   /* saved from the form during a call */
static int      net_dirty;          /* addresses or NAT changed: redo when the line is idle */
static int      log_level = CFG_LOG_INFO, force_debug;
static volatile sig_atomic_t stop;

/* Archive mode (arch/): the gateway program is a child of ours while the
 * setting is on. It listens on these ports; the NAT rules send the callers'
 * web, FTP and name questions there. */
#define STR_(x) #x
#define STR(x) STR_(x)
#define ARCH_HTTP 8380
#define ARCH_FTP 8321
#define ARCH_DNS 8353
#define ARCH_PASV_LO 50000
#define ARCH_PASV_HI 50015
static const char *arch_path = "/usr/sbin/v90arch";
static pid_t    arch_pid;
static int      arch_want;
static uint32_t arch_next_ms;       /* not before this: a gateway that dies is not restarted in a loop */

/* The Firmware page (cfg/firmware.h). Both things it can do write the flash,
 * and one of them first takes a megabyte from a browser: neither is done
 * here. A request that may go ahead is handed, connection and all, to the
 * flash tool of the image (fw/tools/v90nv, nvhttp.c) as a child of ours.
 * What the page shows comes from two small files in memory: what the boot
 * script noted about how the box was started, and what that child noted
 * about its last write. */
static const char *fw_tool = "/usr/sbin/v90nv";
static pid_t    fw_pid;

static void logf_(int level, const char *fmt, ...)
{
    static const char *const tag[] = { "error", "warn", "info", "debug" };
    va_list ap;
    if (level > log_level) return;
    fprintf(stderr, "v90d %s: ", tag[level]);
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

static const char *ip_str(uint32_t ip, char buf[16])
{
    snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 255, (unsigned)(ip >> 8) & 255, (unsigned)ip & 255);
    return buf;
}

static void on_term(int sig) { (void)sig; stop = 1; }

/* an IP packet from the caller: into the kernel */
static void ip_to_kernel(void *ctx, const uint8_t *pkt, size_t len)
{
    int i = (int)(((srv_t *)ctx) - S);
    if (tun_fd[i] >= 0 && write(tun_fd[i], pkt, len) < 0) S[i].status.rx_errors++;
}

/* a port has the archive for its service */
static int any_archive(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].cfg.service == CFG_SERVICE_ARCHIVE) return 1;
    return 0;
}

/* a port has the game console service */
static int any_console(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].cfg.service == CFG_SERVICE_DREAMCAST) return 1;
    return 0;
}

/* The address list of the game revival projects (dclist.h): fetched by a
 * child process while a port has the console service. The child writes the
 * list to its standard output, a pipe we read as it comes (no file, so
 * nothing in /tmp to be put in its way); it is parsed when the child is
 * done and in force from the next time the line is idle. A fetch that fails
 * leaves the list as it was. */
static const char *dc_cmd = "wget -q -T 8 -O - " DCLIST_URL;
static nat_rewrite dc_rw[NAT_REWRITE_MAX]; static int dc_n;
static pid_t    dc_pid; static uint32_t dc_next_ms; static int dc_started;
static int      dc_fd = -1; static char dc_text[8192]; static size_t dc_len;
static void dc_drain(void)
{
    char junk[512]; ssize_t r;
    while (dc_fd >= 0) {
        if (dc_len < sizeof dc_text) r = read(dc_fd, dc_text + dc_len, sizeof dc_text - dc_len);
        else r = read(dc_fd, junk, sizeof junk);            /* more than a list can be: read and dropped */
        if (r <= 0) break;
        if (dc_len < sizeof dc_text) dc_len += (size_t)r;
    }
}
static void dc_poll(uint32_t now_ms)
{
    int st;
    if (dc_pid > 0) {
        dc_drain();
        if (waitpid(dc_pid, &st, WNOHANG) != dc_pid) return;
        dc_drain();
        dc_pid = 0; close(dc_fd); dc_fd = -1;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            static nat_rewrite got[NAT_REWRITE_MAX];
            int n = dclist_parse(dc_text, dc_len, got, NAT_REWRITE_MAX);
            if (n == 0) logf_(CFG_LOG_WARN, "console service: the address list that came holds no rule; keeping %d", dc_n);
            else if (n != dc_n || memcmp(got, dc_rw, (size_t)n * sizeof got[0]) != 0) {
                memcpy(dc_rw, got, (size_t)n * sizeof got[0]); dc_n = n; net_dirty = 1;
                logf_(CFG_LOG_INFO, "console service: address list with %d rules", n);
            }
            dc_next_ms = now_ms + 6u * 3600u * 1000u;
        } else {
            logf_(CFG_LOG_WARN, "console service: the address list could not be fetched; trying again in 5 minutes");
            dc_next_ms = now_ms + 300000u;
        }
        return;
    }
    if (!dc_cmd[0] || !any_console()) return;
    if (dc_started && (int32_t)(now_ms - dc_next_ms) < 0) return;
    dc_started = 1; dc_next_ms = now_ms + 300000u;
    {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) return;
        dc_len = 0;
        dc_pid = fork();
        if (dc_pid == 0) {
            struct sched_param sp;
            memset(&sp, 0, sizeof sp);
            (void)sched_setscheduler(0, SCHED_OTHER, &sp);
            (void)munlockall();
            (void)fcntl(pp[1], F_SETFL, 0);                 /* the child writes without hurry */
            if (dup2(pp[1], 1) < 0) _exit(126);
            execl("/bin/sh", "sh", "-c", dc_cmd, (char *)NULL);
            _exit(127);
        }
        close(pp[1]);
        if (dc_pid < 0) { dc_pid = 0; close(pp[0]); return; }
        dc_fd = pp[0];
    }
}

/* "Dreamcast Now" (dcnow.h): while the caller of a console port that asks
 * for it has its network up, a report every 15 s, sent by a child process
 * (the image's wget; dn_prog, when set, is run instead as "prog URL BODY"). */
static const char *dn_prog;
static struct { dcnow_t d; int on; uint32_t next_ms; pid_t pid; } dn[PORTS];
static void dn_poll(uint32_t now_ms)
{
    int i;
    for (i = 0; i < n_ports; i++) {
        int up = S[i].cfg.service == CFG_SERVICE_DREAMCAST && S[i].cfg.dc_now && srv_ip_up(&S[i]);
        if (dn[i].pid > 0 && waitpid(dn[i].pid, NULL, WNOHANG) == dn[i].pid) dn[i].pid = 0;
        if (!up) { dn[i].on = 0; continue; }
        if (!dn[i].on) {
            uint8_t mac[6]; char path[80], text[32]; unsigned m[6]; int fd, k; ssize_t r = 0;
            memset(mac, 0, sizeof mac);
            snprintf(path, sizeof path, "/sys/class/net/%s/address", lan_if);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) { r = read(fd, text, sizeof text - 1); close(fd); }
            text[r > 0 ? r : 0] = 0;
            if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6)
                for (k = 0; k < 6; k++) mac[k] = (uint8_t)m[k];
            dcnow_start(&dn[i].d, mac);
            dn[i].on = 1; dn[i].next_ms = now_ms;
            logf_(CFG_LOG_INFO, "port %d: reporting to Dreamcast Now while this call lasts", i + 1);
        }
        if ((int32_t)(now_ms - dn[i].next_ms) < 0 || dn[i].pid > 0) continue;
        dn[i].next_ms = now_ms + DCNOW_EVERY_MS;
        {
            char url[160], body[96];
            (void)dcnow_body(&dn[i].d, console_name(&S[i].console, NULL), body, sizeof body);
            snprintf(url, sizeof url, "https://" DCNOW_HOST "%s", dcnow_path(&dn[i].d));
            dn[i].pid = fork();
            if (dn[i].pid == 0) {
                struct sched_param sp;
                memset(&sp, 0, sizeof sp);
                (void)sched_setscheduler(0, SCHED_OTHER, &sp);
                (void)munlockall();
                if (dn_prog) execl(dn_prog, dn_prog, url, body, (char *)NULL);
                else execlp("wget", "wget", "-q", "-T", "15", "-O", "/dev/null", "-U", DCNOW_AGENT, "--post-data", body, url, (char *)NULL);
                _exit(127);
            }
            if (dn[i].pid < 0) dn[i].pid = 0;
        }
    }
}

/* A console as a host on the network (dreamcast_lan): the caller of that
 * port gets an address of the network itself. The address comes from a
 * command that prints it (on the box, udhcpc asking the network's DHCP
 * server for a second lease under a client name of its own; the script it
 * is given prints the address and changes nothing). It is asked again every
 * half hour, which also keeps the lease. Without an answer the port stays
 * as the settings have it, behind this box's address. */
static const char *la_cmd;              /* -E; null = udhcpc on lan_if */
static struct { uint32_t ip; pid_t pid; int fd; uint32_t next_ms; int started; char text[64]; size_t len; } la[PORTS];
static int lan_wanted(int i) { return S[i].cfg.service == CFG_SERVICE_DREAMCAST && S[i].cfg.dc_lan; }
static int any_lan(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (lan_wanted(i) && la[i].ip) return 1;
    return 0;
}
static void la_poll(uint32_t now_ms)
{
    int i, st;
    for (i = 0; i < n_ports; i++) {
        if (la[i].pid > 0) {
            ssize_t r;
            while (la[i].len < sizeof la[i].text - 1 && (r = read(la[i].fd, la[i].text + la[i].len, sizeof la[i].text - 1 - la[i].len)) > 0)
                la[i].len += (size_t)r;
            if (waitpid(la[i].pid, &st, WNOHANG) != la[i].pid) continue;
            la[i].pid = 0; close(la[i].fd); la[i].fd = -1;
            la[i].text[la[i].len] = 0;
            la[i].text[strcspn(la[i].text, " \r\n")] = 0;
            {
                struct in_addr ia; uint32_t ip = 0; char a[16];
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && inet_aton(la[i].text, &ia)) ip = ntohl(ia.s_addr);
                if (!ip) {
                    logf_(CFG_LOG_WARN, "port %d: no address for the console from the network; trying again in a minute", i + 1);
                    la[i].next_ms = now_ms + 60000u;
                } else {
                    la[i].next_ms = now_ms + 1800000u;
                    if (ip != la[i].ip) {
                        la[i].ip = ip; net_dirty = 1;
                        logf_(CFG_LOG_INFO, "port %d: the console is %s on the network", i + 1, ip_str(ip, a));
                    }
                }
            }
            continue;
        }
        if (!lan_wanted(i)) { if (la[i].ip) { la[i].ip = 0; net_dirty = 1; } la[i].started = 0; continue; }
        if (la[i].started && (int32_t)(now_ms - la[i].next_ms) < 0) continue;
        la[i].started = 1; la[i].next_ms = now_ms + 60000u;
        {
            int pp[2]; char cmd[256];
            if (la_cmd) snprintf(cmd, sizeof cmd, "%s", la_cmd);
            else snprintf(cmd, sizeof cmd, "udhcpc -i %s -f -n -q -t 4 -T 2 -x 0x3d:007639306464632d%02x -s /etc/dclease.script 2>/dev/null",
                          lan_if, 0x31 + i);        /* client name "v90ddc-1", "v90ddc-2" */
            if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) continue;
            la[i].len = 0;
            la[i].pid = fork();
            if (la[i].pid == 0) {
                struct sched_param sp;
                memset(&sp, 0, sizeof sp);
                (void)sched_setscheduler(0, SCHED_OTHER, &sp);
                (void)munlockall();
                (void)fcntl(pp[1], F_SETFL, 0);
                if (dup2(pp[1], 1) < 0) _exit(126);
                execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
                _exit(127);
            }
            close(pp[1]);
            if (la[i].pid < 0) { la[i].pid = 0; close(pp[0]); continue; }
            la[i].fd = pp[0];
        }
    }
}
/* the network side answers ARP for the console's address, or not */
static void proxy_arp(int on)
{
    char path[80]; int fd;
    snprintf(path, sizeof path, "/proc/sys/net/ipv4/conf/%s/proxy_arp", lan_if);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) { if (on) logf_(CFG_LOG_WARN, "cannot switch proxy ARP on %s: %s", lan_if, strerror(errno)); return; }
    if (write(fd, on ? "1\n" : "0\n", 2) != 2 && on) logf_(CFG_LOG_WARN, "cannot switch proxy ARP on %s: %s", lan_if, strerror(errno));
    close(fd);
}

static int all_onhook(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].tel.state != TEL_ONHOOK) return 0;
    return 1;
}

static void arch_stop(void)
{
    if (arch_pid > 0) { kill(arch_pid, SIGTERM); (void)waitpid(arch_pid, NULL, 0); arch_pid = 0; }
}

/* start, stop or restart the gateway as wanted; called on a change and every few seconds */
static void arch_poll(uint32_t now_ms)
{
    if (arch_pid > 0 && waitpid(arch_pid, NULL, WNOHANG) == arch_pid) {
        arch_pid = 0;
        if (arch_want) logf_(CFG_LOG_WARN, "archive mode: the gateway stopped; starting it again");
    }
    if (!arch_want) { arch_stop(); return; }
    if (arch_pid > 0 || !arch_path[0] || (int32_t)(now_ms - arch_next_ms) < 0) return;
    arch_next_ms = now_ms + 5000;
    arch_pid = fork();
    if (arch_pid == 0) {
        /* the child is an ordinary process: no real-time priority, no locked memory */
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        (void)sched_setscheduler(0, SCHED_OTHER, &sp);
        (void)munlockall();
        /* every name the gateway gives out is in this block: what is not
         * redirected to it (a secure site, mail) is refused at once instead
         * of waiting for an answer that cannot come */
        if (system("ip route replace unreachable 198.18.0.0/15 2>/dev/null") != 0) { /* the gateway works without it */ }
        execl(arch_path, arch_path, "-w", STR(ARCH_HTTP), "-f", STR(ARCH_FTP), "-d", STR(ARCH_DNS),
              "-p", STR(ARCH_PASV_LO) "-" STR(ARCH_PASV_HI), (char *)NULL);
        _exit(127);
    }
    if (arch_pid < 0) { arch_pid = 0; logf_(CFG_LOG_ERROR, "archive mode: cannot start %s: %s", arch_path, strerror(errno)); }
    else logf_(CFG_LOG_INFO, "archive mode: %s started", arch_path);
}
static uint32_t arch_clock;
static void arch_apply(int on) { arch_want = on; arch_next_ms = arch_clock; arch_poll(arch_clock); }

static int fw_text(void *ctx, int which, char *buf, size_t cap)
{
    const char *path = which == HTTPD_FW_WHERE ? getenv("V90D_WHERE") : getenv("V90NV_STATE");
    ssize_t n;
    int fd;
    (void)ctx;
    if (!path) path = which == HTTPD_FW_WHERE ? "/tmp/v90d.where" : "/tmp/v90d.fwstate";
    buf[0] = '\0';
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0) return 0;
    n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static const char *fw_take(void *ctx, int what, int fd, const char *early, size_t early_len, size_t content_length, const char *ctype)
{
    char len[24];
    int pfd[2], i;
    (void)ctx;
    /* flash writes hold the processor in bursts: not under a call */
    if (!all_onhook()) return "A call is in progress. Try again when the lines are free.";
    if (fw_pid > 0 && waitpid(fw_pid, NULL, WNOHANG) == 0) return "The flash is being written already. Wait until that is done.";
    fw_pid = 0;
    /* what we have of the body goes ahead through a pipe (it holds 64 kB,
     * the request buffer is two) */
    if (pipe(pfd) < 0) return "The box cannot start its flash tool just now.";
    if (early_len && write(pfd[1], early, early_len) != (ssize_t)early_len) { close(pfd[0]); close(pfd[1]); return "The box cannot start its flash tool just now."; }
    snprintf(len, sizeof len, "%lu", (unsigned long)content_length);
    fw_pid = fork();
    if (fw_pid == 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        (void)sched_setscheduler(0, SCHED_OTHER, &sp);
        (void)munlockall();
        if (dup2(fd, 0) < 0 || dup2(fd, 1) < 0 || dup2(pfd[0], 3) < 0) _exit(127);
        for (i = 4; i < 256; i++) close(i);     /* the lines, the listener: none of its business */
        (void)setenv("V90NV_EARLY_FD", "3", 1);
        execl(fw_tool, fw_tool, what == HTTPD_FW_UPLOAD ? "serve-upgrade" : "serve-stock", len,
              what == HTTPD_FW_UPLOAD ? ctype : (char *)NULL, (char *)NULL);
        _exit(127);
    }
    close(pfd[0]); close(pfd[1]);
    if (fw_pid < 0) { fw_pid = 0; return "The box cannot start its flash tool just now."; }
    logf_(CFG_LOG_INFO, "firmware page: %s handed to %s", what == HTTPD_FW_UPLOAD ? "an upload" : "back to stock", fw_tool);
    return NULL;
}

/* TUN devices, forwarding and NAT as the configuration in force wants them */
static void net_apply(void)
{
    char a[16], b[16];
    int i;
    net_dirty = 0;
    for (i = 0; i < n_ports; i++) {
        cfg_t *c = &S[i].cfg;
        if (lan_wanted(i) && la[i].ip && c->ppp_peer != la[i].ip) {
            static cfg_t v;
            v = *c; v.ppp_peer = la[i].ip;
            srv_reconfigure(&S[i], &v);
        }
        if (tun_fd[i] >= 0) close(tun_fd[i]);
        tun_fd[i] = tun_open(tun_name[i], c->ppp_local, c->ppp_peer, c->mru);
        if (tun_fd[i] < 0) logf_(CFG_LOG_ERROR, "cannot set up %s: %s (callers will get PPP but no network)", tun_name[i], strerror(errno));
        else logf_(CFG_LOG_INFO, "%s up: %s -> %s", tun_name[i], ip_str(c->ppp_local, a), ip_str(c->ppp_peer, b));
    }
    if (nat_ip_forward(1) < 0) logf_(CFG_LOG_WARN, "cannot enable IP forwarding: %s", strerror(errno));
    if (S[0].cfg.nat || any_archive() || any_console()) {
        nat_plan p;
        uint32_t p2 = n_ports > 1 ? S[1].cfg.ppp_peer : 0;
        memset(&p, 0, sizeof p);
        p.peer[0] = S[0].cfg.ppp_peer; p.peer[1] = p2;
        p.masquerade = S[0].cfg.nat;
        for (i = 0; i < n_ports; i++) p.archive[i] = S[i].cfg.service == CFG_SERVICE_ARCHIVE;
        for (i = 0; i < n_ports; i++) p.console[i] = S[i].cfg.service == CFG_SERVICE_DREAMCAST;
        for (i = 0; i < n_ports; i++) p.lan[i] = lan_wanted(i) && la[i].ip;
        p.rewrite = dc_rw; p.n_rewrite = dc_n;
        p.n_open = dclist_ports(&p.open);
        strncpy(p.wan, lan_if, sizeof p.wan - 1);
        p.http_port = ARCH_HTTP; p.ftp_port = ARCH_FTP; p.dns_port = ARCH_DNS; p.pasv_lo = ARCH_PASV_LO; p.pasv_hi = ARCH_PASV_HI;
        if (nat_apply(&p) < 0) logf_(CFG_LOG_ERROR, "cannot install the NAT rules: %s", strerror(errno));
        else {
            if (p.masquerade && p2) logf_(CFG_LOG_INFO, "NAT: %s and %s are masqueraded", ip_str(S[0].cfg.ppp_peer, a), ip_str(p2, b));
            else if (p.masquerade) logf_(CFG_LOG_INFO, "NAT: %s is masqueraded", ip_str(S[0].cfg.ppp_peer, a));
            for (i = 0; i < n_ports; i++)
                if (p.archive[i]) logf_(CFG_LOG_INFO, "archive mode: the web, FTP and name questions of port %d go to the gateway", i + 1);
            for (i = 0; i < n_ports; i++)
                if (p.console[i]) logf_(CFG_LOG_INFO, "console service on port %d: %d address rules, %d port ranges opened from %s", i + 1, dc_n, p.n_open, lan_if);
        }
    } else if (nat_remove() < 0 && errno != ENOENT) {
        logf_(CFG_LOG_WARN, "cannot remove the NAT rule: %s", strerror(errno));
    }
    proxy_arp(any_lan());
    arch_apply(any_archive());
}

/* a new configuration is in force from the next call */
static void adopt(const cfg_t *n)
{
    static cfg_t view;
    int i;
    if (!force_debug) log_level = n->log_level;
    for (i = 0; i < n_ports; i++) {
        cfg_for_port(n, i, &view);
        if (view.service == CFG_SERVICE_DREAMCAST && view.dc_lan && la[i].ip) view.ppp_peer = la[i].ip;
        if (view.dc_lan != S[i].cfg.dc_lan) net_dirty = 1;
        if (view.ppp_local != S[i].cfg.ppp_local || view.ppp_peer != S[i].cfg.ppp_peer || view.nat != S[i].cfg.nat || view.service != S[i].cfg.service || view.mru != S[i].cfg.mru)
            net_dirty = 1;
        srv_reconfigure(&S[i], &view);
    }
}

static int store(const cfg_t *n)
{
    char err[CFG_ERR_MAX];
    if (cfg_save(n, cfg_path, err, sizeof err) != CFG_OK) {
        logf_(CFG_LOG_ERROR, "settings not saved: %s", err);
        return -1;
    }
    logf_(CFG_LOG_INFO, "settings saved to %s; they apply from the next call", cfg_path);
    return 0;
}

/* The settings page was posted. Writing the file can stall for as long as
 * the flash takes, and a call does not survive missing its samples: while
 * the line is in use the file is written after the call. */
static int on_save(void *ctx, const cfg_t *n)
{
    (void)ctx;
    if (!all_onhook()) {
        unsaved = *n; have_unsaved = 1;
        adopt(n);
        logf_(CFG_LOG_INFO, "settings accepted; the file is written when the lines are free");
        return 0;
    }
    if (store(n) != 0) return -1;
    have_unsaved = 0;
    adopt(n);
    return 0;
}

static const char *const tel_name[] = { "answered", "busy", "wrong number", "abandoned", "line dropped", "session limit", "idle timeout" };
static const char *const mod_name[] = { "no carrier", "V.90", "V.34", "V.32bis", "V.22bis", "V.22", "V.21", "V.92" };

/* PPP compression (CCP) of the call that ended: one line, only if it was used */
static void log_ppp_comp(const char *who, const ppp_t *ppp)
{
    static const char *const alg[] = { "none", "Deflate", "MPPC" };
    ppp_status_t ps;
    const ppp_counters_t *k = &ps.counters;
    ppp_get_status(ppp, &ps);
    if (!k->ccp_opens) return;
    logf_(CFG_LOG_INFO, "%s  PPP compression: to caller %s %lu -> %lu octets (%lu packets sent as they were), from caller %s %lu -> %lu;"
          " resets asked of us %lu, by us %lu (%lu packets dropped), CCP opened %lu time(s)",
          who, alg[k->comp_tx < 3 ? k->comp_tx : 0], (unsigned long)k->comp_tx_in, (unsigned long)k->comp_tx_out,
          (unsigned long)k->comp_tx_native, alg[k->comp_rx < 3 ? k->comp_rx : 0],
          (unsigned long)k->comp_rx_in, (unsigned long)k->comp_rx_out,
          (unsigned long)k->comp_tx_resets, (unsigned long)k->comp_rx_errors, (unsigned long)k->comp_rx_dropped,
          (unsigned long)k->ccp_opens);
}

static void log_last_call(int port)
{
    const st_call_t *h = status_call(&S[port].status, 0);
    char who[12] = "";
    if (!h) return;
    if (n_ports > 1) snprintf(who, sizeof who, "port %d: ", port + 1);
    logf_(CFG_LOG_INFO, "%scall to \"%s\": %s; %s %u/%u bit/s, %u s",
          who, h->dialed, (unsigned)h->tel_result < 7 ? tel_name[h->tel_result] : "?",
          (unsigned)h->modulation < 8 ? mod_name[h->modulation] : "?",
          (unsigned)h->down_bps, (unsigned)h->up_bps, (unsigned)h->duration_s);
    if (h->modulation) {
        /* the call object is left as it ended: what the link layer did */
        v42ec_status_t v;
        char comp[96];
        v42ec_get_status(&S[port].dce.v42, &v);
        /* send/receive direction; V.44 has its values per direction */
        if (v.comp_alg == V42EC_COMP_V44)
            snprintf(comp, sizeof comp, "V.44 %s/%s %d/%d codewords, string %d/%d, history %d/%d",
                     v.comp_tx ? "on" : "off", v.comp_rx ? "on" : "off", v.v44_n2_tx, v.v44_n2_rx,
                     v.v44_n7_tx, v.v44_n7_rx, v.v44_n8_tx, v.v44_n8_rx);
        else
            snprintf(comp, sizeof comp, "V.42bis %s/%s %d codewords, string %d",
                     v.comp_tx ? "on" : "off", v.comp_rx ? "on" : "off", v.comp_n2, v.comp_n7);
        logf_(CFG_LOG_INFO, "%s  link: k %d/%d, N401 %d/%d, %s; I frames %lu sent (%lu again), %lu received (%lu dropped), REJ %lu sent %lu received, T401 ran out %lu times; compressor %lu -> %lu octets, expander %lu -> %lu",
              who, v.k_tx, v.k_rx, v.n401_tx, v.n401_rx, comp,
              v.tx_i, v.tx_i_retx, v.rx_i, v.rx_i_dropped, v.tx_rej, v.rx_rej, v.t401_expiries, v.comp_in, v.comp_out, v.decomp_in, v.decomp_out);
        log_ppp_comp(who, &S[port].dce.ppp);
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: v90d [-c FILE] -l LINE [-l LINE2] [-d N] [-i LAN-IF] [-t TUN] [-A GATEWAY] [-D FETCH-COMMAND] [-G REPORT-PROGRAM] [-E LEASE-COMMAND] [-v]\n       v90d [-c FILE] -p\n"
                    "bench: [-R l|c per port, e.g. lc] [-P 1..31: maximum downstream power code] [-w|-W FILE: record port 1|2]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    static uint8_t pending[PORTS][1600]; static ssize_t pending_n[PORTS];
    static cfg_t view;
    const char *line_spec[PORTS] = { NULL, NULL };
    char err[CFG_ERR_MAX];
    line_t line[PORTS];
    struct pollfd pfd[PORTS];
    uint8_t rx[LINE_BLOCK], tx[LINE_BLOCK];
    int16_t lin[LINE_BLOCK]; int is_lin = 0, blk_lin[PORTS] = { 0, 0 }, cap_lin = 0, max_tx_code = 0;
    const char *rx_formats = "";        /* -R */
    uint32_t seed = 0, now_ms = 0, listen_addr = 0, calls_seen[PORTS] = { 0, 0 };
    int opt, print_net = 0, line_law[PORTS] = { 0, 0 }, r, offhook = 0, i, p, blocks = 0, echo_delay = -1, listen_warned = 0;
    struct sigaction sa;
    const char *cap_path = NULL; int cap_fd = -1, cap_port = 0; long cap_n = 0;
    /* the telephone lights: <prefix><port>/brightness, lit while the port is off hook */
    const char *led_prefix = "/sys/class/leds/ht802:phone"; int led_fd[PORTS] = { -1, -1 }, led_on[PORTS] = { -1, -1 };

    while ((opt = getopt(argc, argv, "c:l:d:i:t:vpw:W:L:A:N:R:P:D:G:E:")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'l': if (n_ports >= PORTS) usage(); line_spec[n_ports++] = optarg; break;
        case 'd': echo_delay = atoi(optarg); if (echo_delay < 0 || echo_delay > 1900) usage(); break;
        case 'i': lan_if = optarg; break;
        case 't': {
            size_t n = strlen(optarg);
            if (n == 0 || n >= sizeof tun_name[0] - 1) usage();
            strcpy(tun_name[0], optarg); strcpy(tun_name[1], optarg);
            /* the second port's device: the next digit, or a 1 appended */
            if (optarg[n - 1] >= '0' && optarg[n - 1] < '9') tun_name[1][n - 1]++;
            else { tun_name[1][n] = '1'; tun_name[1][n + 1] = '\0'; }
            break;
        }
        case 'v': force_debug = 1; log_level = CFG_LOG_DEBUG; break;
        case 'p': print_net = 1; break;
        case 'L': led_prefix = optarg; break;
        case 'N': fw_tool = optarg; break;      /* the flash tool behind the Firmware page */
        case 'E': la_cmd = optarg; break;       /* prints the console's address on the network (instead of udhcpc) */
        case 'G': dn_prog = optarg; break;      /* run instead of wget for the Dreamcast Now report: PROG URL BODY */
        case 'D': dc_cmd = optarg; break;       /* the command that fetches the console address list; "" = none */
        case 'A': arch_path = optarg; break;  /* the archive gateway program; "" = do not start one */
        case 'R': rx_formats = optarg; if (strlen(optarg) > PORTS || strspn(optarg, "lc") != strlen(optarg)) usage(); break;
        case 'P': max_tx_code = atoi(optarg); if (max_tx_code < 1 || max_tx_code > 31) usage(); break;
        case 'w': cap_path = optarg; break;
        case 'W': cap_path = optarg; cap_port = 1; break;    /* the same for port 2 */   /* bring-up: record port 1's line while off hook */
        default: usage();
        }
    }
    r = cfg_load(&live, cfg_path, err, sizeof err);
    if (r == CFG_ERR) { fprintf(stderr, "v90d: %s\n", err); return 1; }
    if (print_net) {
        char a[16], b[16], g[16], d1[16], d2[16];
        printf("LAN_MODE=%s\nLAN_ADDRESS=%s\nLAN_NETMASK=%s\n", live.lan_dhcp ? "dhcp" : "static",
               ip_str(live.lan_addr, a), ip_str(live.lan_netmask, b));
        /* empty = none */
        printf("LAN_GATEWAY=%s\nLAN_DNS1=%s\nLAN_DNS2=%s\n", live.lan_gateway ? ip_str(live.lan_gateway, g) : "",
               live.dns[0] ? ip_str(live.dns[0], d1) : "", live.dns[1] ? ip_str(live.dns[1], d2) : "");
        return 0;
    }
    if (!n_ports) usage();
    if (!force_debug) log_level = live.log_level;
    if (r == CFG_DEFAULTS) logf_(CFG_LOG_WARN, "%s not found: running on the defaults", cfg_path);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = SIG_IGN; sigaction(SIGPIPE, &sa, NULL);

    i = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (i >= 0) { if (read(i, &seed, sizeof seed) != (ssize_t)sizeof seed) seed = 0; close(i); }
    if (!seed) seed = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);

    for (p = 0; p < n_ports; p++) {
        if (line_open(&line[p], line_spec[p]) < 0) { logf_(CFG_LOG_ERROR, "line %s: %s", line_spec[p], strerror(errno)); return 1; }
        cfg_for_port(&live, p, &view);
        line_law[p] = view.law == CFG_LAW_ALAW;
        if (line_set_law(&line[p], line_law[p]) < 0) logf_(CFG_LOG_WARN, "line %s: cannot set the law: %s", line_spec[p], strerror(errno));
        srv_init(&S[p], &view, seed + 0x9E3779B9u * (uint32_t)p, ip_to_kernel, &S[p]);
        srv_set_echo_delay(&S[p], echo_delay);
        srv_set_max_tx_code(&S[p], max_tx_code);
        if ((size_t)p < strlen(rx_formats)) {
            if (line_set_linear(&line[p], rx_formats[p] == 'l') < 0)
                logf_(CFG_LOG_WARN, "line %s: cannot set the receive format: %s", line_spec[p], strerror(errno));
        } else if (line[p].dev && line_set_linear(&line[p], 1) < 0) {
            /* a driver from before the linear format: companded it stays */
            logf_(CFG_LOG_INFO, "line %s: no linear receive (%s), staying companded", line_spec[p], strerror(errno));
        }
        pfd[p].fd = line[p].fd; pfd[p].events = POLLIN;
        logf_(CFG_LOG_INFO, "port %d ready on line %s; number \"%s\"", p + 1, line_spec[p], view.line_number[0] ? view.line_number : "any");
    }
    net_apply();
    httpd_init(&H, &live, &S[0].status);
    if (n_ports > 1) httpd_status2(&H, &S[1].status);
    httpd_on_save(&H, on_save, NULL);
    {
        static const httpd_fw_t fw = { V90D_VERSION " (built " __DATE__ ")", fw_text, fw_take, NULL };
        httpd_on_firmware(&H, &fw);
    }

    /* The line gives us 10 ms blocks and wants each answered in time; a late
     * one costs the caller's modem its training. So: ahead of everything
     * else on the box, and never waiting for a page. */
    {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp); sp.sched_priority = 50;
        if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) logf_(CFG_LOG_WARN, "no real-time priority: %s", strerror(errno));
        if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) logf_(CFG_LOG_WARN, "cannot lock the memory: %s", strerror(errno));
    }

    for (p = 0; p < n_ports; p++) {
        char path[300];
        snprintf(path, sizeof path, "%s%d/brightness", led_prefix, p + 1);
        led_fd[p] = open(path, O_WRONLY);       /* no such light: nothing is shown */
    }

    while (!stop) {
        /* the next block of either line; the first line's blocks count the time */
        r = poll(pfd, (nfds_t)n_ports, 1000);
        if (r < 0) { if (errno == EINTR) continue; logf_(CFG_LOG_ERROR, "poll: %s", strerror(errno)); break; }
        if (r == 0) continue;
        for (p = 0; p < n_ports && !stop; p++) {
            if (!(pfd[p].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            r = line_read2(&line[p], rx, lin, &is_lin, &offhook);
            if (r == 0) { logf_(CFG_LOG_INFO, "line closed"); stop = 1; break; }
            if (r < 0) { logf_(CFG_LOG_ERROR, "line: %s", strerror(errno)); stop = 1; break; }
            if (led_fd[p] >= 0 && (offhook != 0) != led_on[p]) {
                led_on[p] = offhook != 0;
                if (pwrite(led_fd[p], led_on[p] ? "1\n" : "0\n", 2, 0) < 0) { close(led_fd[p]); led_fd[p] = -1; }
            }
            if (is_lin != blk_lin[p]) {
                blk_lin[p] = is_lin;
                srv_set_rx_linear(&S[p], is_lin);      /* from the next call on */
                logf_(CFG_LOG_INFO, "port %d: the line now gives %s samples", p + 1, is_lin ? "16-bit linear" : "companded (G.711)");
            }
            if (is_lin) for (i = 0; i < LINE_BLOCK; i++) tx[i] = srv_tick_lin(&S[p], offhook, lin[i]);
            else for (i = 0; i < LINE_BLOCK; i++) tx[i] = srv_tick(&S[p], offhook, rx[i]);
            if (p == cap_port && cap_path) {
                /* 80 codewords heard, then the 80 sent in answer, per block; 8 MB at most */
                if (offhook && cap_fd < 0 && cap_n == 0) {
                    cap_fd = open(cap_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    cap_lin = is_lin;                   /* a recording is of one kind */
                    if (cap_fd >= 0 && cap_lin && write(cap_fd, "V90DLIN1", 8) < 0) cap_n = 50000;
                }
                if (cap_fd >= 0 && offhook && cap_n < 50000 && is_lin != cap_lin) cap_n = 50000;
                if (cap_fd >= 0 && offhook && cap_n < 50000) {
                    uint8_t heard[2 * LINE_BLOCK]; size_t hn = LINE_BLOCK;
                    if (!is_lin) memcpy(heard, rx, LINE_BLOCK);
                    else for (hn = 0; hn < 2 * LINE_BLOCK; hn += 2) { heard[hn] = (uint8_t)((uint16_t)lin[hn / 2] & 255); heard[hn + 1] = (uint8_t)((uint16_t)lin[hn / 2] >> 8); }
                    if (write(cap_fd, heard, hn) < 0 || write(cap_fd, tx, LINE_BLOCK) < 0) cap_n = 50000;
                    cap_n++;
                }
                if (!offhook && cap_fd >= 0) { close(cap_fd); cap_fd = -1; }
                if (!offhook && cap_n && access(cap_path, F_OK) != 0) cap_n = 0;   /* file taken away: record the next call */
            }
            if (line_write(&line[p], tx) < 0) { logf_(CFG_LOG_ERROR, "line: %s", strerror(errno)); stop = 1; break; }
            /* packets for the caller; one that does not fit yet is kept */
            if (tun_fd[p] >= 0) {
                for (i = 0; i < 16; i++) {
                    if (pending_n[p] <= 0) pending_n[p] = read(tun_fd[p], pending[p], sizeof pending[p]);
                    if (pending_n[p] <= 0) break;
                    r = srv_send_ip(&S[p], pending[p], (size_t)pending_n[p]);
                    if (r == PPP_ERR_FULL) break;
                    pending_n[p] = 0;               /* sent, or nobody to send it to */
                }
            }
            if (S[p].status.calls_total != calls_seen[p]) { calls_seen[p] = S[p].status.calls_total; log_last_call(p); }
            if (S[p].tel.state == TEL_ONHOOK) {
                cfg_for_port(&live, p, &view);
                if (line_law[p] != (view.law == CFG_LAW_ALAW)) { line_law[p] = view.law == CFG_LAW_ALAW; (void)line_set_law(&line[p], line_law[p]); }
            }
            if (p != 0) continue;

            /* once per block of the first line: everything that is not a line */
            now_ms += 10;
            if (all_onhook()) {
                if (have_unsaved) { have_unsaved = 0; (void)store(&unsaved); }
                if (net_dirty) net_apply();
            }
            arch_clock = now_ms;
            if (now_ms % 2000 == 0) arch_poll(now_ms);
            if (now_ms % 1000 == 0) { dc_poll(now_ms); dn_poll(now_ms); la_poll(now_ms); }
            if (fw_pid > 0 && now_ms % 1000 == 0 && waitpid(fw_pid, NULL, WNOHANG) == fw_pid) fw_pid = 0;
            /* the Restart button, once its answer has gone out */
            if (H.restart_asked && httpd_active(&H) == 0) {
                int fd;
                H.restart_asked = 0;
                logf_(CFG_LOG_INFO, "restart asked on the settings page");
                /* The daemon does not restart the machine it runs on: it leaves a
                 * note, and the image's boot script (fw/rcS) keeps what must be
                 * kept and restarts. */
                fd = open(RESTART_NOTE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (fd >= 0) close(fd);
                else logf_(CFG_LOG_WARN, "cannot write %s: %s", RESTART_NOTE, strerror(errno));
            }
            /* once a second: wall clock, and the LAN address may have changed (DHCP) */
            if (++blocks >= 100) {
                time_t t = time(NULL);
                blocks = 0;
                for (i = 0; i < n_ports; i++)
                    if (t > 1600000000 && S[i].tel.state == TEL_ONHOOK) srv_set_clock(&S[i], (uint32_t)t);
                if (live.status_listen == CFG_LISTEN_LAN) {
                    uint32_t a = if_address(lan_if);
                    if (a != listen_addr || (a && H.listen_fd < 0)) {
                        char b[16];
                        httpd_close(&H); listen_addr = 0;
                        /* on every address of the LAN interface where the system lets us
                         * tie the socket to it (so also on the image's rescue address),
                         * else on its address */
                        if (a && (httpd_listen(&H, 0, (uint16_t)live.status_port, lan_if) == 0 ||
                                  httpd_listen(&H, a, (uint16_t)live.status_port, lan_if) == 0)) {
                            listen_addr = a; listen_warned = 0;
                            logf_(CFG_LOG_INFO, "status and settings on http://%s:%d/", ip_str(a, b), live.status_port);
                        } else if (a && !listen_warned) {
                            listen_warned = 1;
                            logf_(CFG_LOG_WARN, "cannot listen on %s:%d: %s", ip_str(a, b), live.status_port, strerror(errno));
                        }
                    }
                } else if (H.listen_fd >= 0) {
                    httpd_close(&H); listen_addr = 0;
                }
            }
            httpd_poll(&H, now_ms);
        }
    }

    httpd_close(&H);
    arch_stop();
    if (S[0].cfg.nat || any_archive() || any_console()) (void)nat_remove();
    if (any_lan()) proxy_arp(0);
    for (p = 0; p < n_ports; p++) line_close(&line[p]);
    logf_(CFG_LOG_INFO, "stopped");
    return 0;
}
