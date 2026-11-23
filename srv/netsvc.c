/* See netsvc.h. The daemon's network layer, extracted from srv/main.c
 * unchanged (0.2.3): the per-port services' background work (the address
 * list, Dreamcast Now, the console's DHCP lease, UPnP, the IWANGO VPN, the
 * archive gateway) and net_apply(), which turns the current settings and
 * that work into the TUN devices, the nftables plan, proxy ARP and the
 * gateway. main.c owns the line, the web server and the lifecycle, and the
 * symbols in daemon.h (S[], the TUN fds, the log). Daemon only. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "daemon.h"
#include "netsvc.h"
#include "tun.h"
#include "nat.h"
#include "dclist.h"
#include "dcnow.h"
#include "upnp.h"
#include "settings.h"
#include "vpn.h"

#define STR_(x) #x
#define STR(x) STR_(x)

int             net_dirty;              /* shared with main.c (daemon.h) */          /* addresses or NAT changed: redo when the line is idle */
#define ARCH_HTTP 8380
#define ARCH_FTP 8321
#define ARCH_DNS 8353
#define ARCH_PASV_LO 50000
#define ARCH_PASV_HI 50015
static const char *arch_path = "/usr/sbin/v90arch";
static pid_t    arch_pid;
static int      arch_want;
static uint32_t arch_next_ms;       /* not before this: a gateway that dies is not restarted in a loop */
static int any_archive(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].cfg.service == CFG_SERVICE_ARCHIVE) return 1;
    return 0;
}
static int any_console(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].cfg.service == CFG_SERVICE_DREAMCAST) return 1;
    return 0;
}
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
static const char *la_cmd;              /* -E; null = udhcpc on lan_if */
static uint32_t peer_conf[PORTS];       /* the caller's address as the settings have it */
static struct { uint32_t ip; pid_t pid; int fd; uint32_t next_ms; int started; char text[64]; size_t len;
                uint32_t until_ms; int timed; } la[PORTS];     /* timed: the lease ends at until_ms */
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
        /* a lease that ran out without being renewed is not ours any more:
         * the network may give the address to somebody else */
        if (la[i].ip && la[i].timed && (int32_t)(now_ms - la[i].until_ms) >= 0) {
            la[i].ip = 0; la[i].timed = 0; net_dirty = 1;
            logf_(CFG_LOG_WARN, "port %d: the console's address on the network ran out and was not renewed; the port is behind this box's address again", i + 1);
        }
        if (la[i].pid > 0) {
            ssize_t r;
            while (la[i].len < sizeof la[i].text - 1 && (r = read(la[i].fd, la[i].text + la[i].len, sizeof la[i].text - 1 - la[i].len)) > 0)
                la[i].len += (size_t)r;
            if (waitpid(la[i].pid, &st, WNOHANG) != la[i].pid) continue;
            la[i].pid = 0; close(la[i].fd); la[i].fd = -1;
            la[i].text[la[i].len] = 0;
            {
                struct in_addr ia; uint32_t ip = 0; char a[16]; unsigned long secs = 0;
                char *sp = la[i].text + strcspn(la[i].text, " \r\n");
                if (*sp == ' ') secs = strtoul(sp + 1, NULL, 10);     /* "address seconds": how long the lease is */
                *sp = 0;
                if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && inet_aton(la[i].text, &ia)) ip = ntohl(ia.s_addr);
                if (!ip) {
                    uint32_t again = 60000u;
                    if (la[i].ip && la[i].timed && (uint32_t)(la[i].until_ms - now_ms) / 4u < again) again = (uint32_t)(la[i].until_ms - now_ms) / 4u + 1000u;
                    logf_(CFG_LOG_WARN, "port %d: no address for the console from the network; trying again", i + 1);
                    la[i].next_ms = now_ms + again;
                } else {
                    uint32_t again = 1800000u;
                    la[i].timed = 0;
                    if (secs && secs < 40u * 24u * 3600u) {
                        la[i].timed = 1; la[i].until_ms = now_ms + (uint32_t)secs * 1000u;
                        if (secs * 500u < again) again = (uint32_t)(secs * 500u);      /* at half time */
                    }
                    la[i].next_ms = now_ms + again;
                    if (ip != la[i].ip) {
                        la[i].ip = ip; net_dirty = 1;
                        logf_(CFG_LOG_INFO, "port %d: the console is %s on the network", i + 1, ip_str(ip, a));
                    }
                }
            }
            continue;
        }
        if (!lan_wanted(i)) { if (la[i].ip) { la[i].ip = 0; la[i].timed = 0; net_dirty = 1; } la[i].started = 0; continue; }
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
static pid_t    up_pid; static uint32_t up_ip, up_next_ms; static int up_dirty = 1;
static void upnp_child(int add, uint32_t ip)
{
    static uint8_t proto[NAT_OPEN_MAX]; static uint16_t lo[NAT_OPEN_MAX], hi[NAT_OPEN_MAX];
    const nat_open *t; int n = dclist_ports(&t), i, r; struct sched_param sp; char a[16];
    /* this child does not become another program: it must let go of the
     * daemon's lines, network devices and listening sockets itself */
    for (i = 3; i < 1024; i++) (void)close(i);
    memset(&sp, 0, sizeof sp);
    (void)sched_setscheduler(0, SCHED_OTHER, &sp);
    (void)munlockall();
    for (i = 0; i < n; i++) { proto[i] = t[i].proto; lo[i] = t[i].lo; hi[i] = t[i].hi; }
    r = upnp_run(if_address(lan_if), add, ip, proto, lo, hi, n, "v90d: Dreamcast");
    if (r == -1) logf_(CFG_LOG_WARN, "game ports: no router answered the search (UPnP)");
    else if (r == -2) logf_(CFG_LOG_WARN, "game ports: the router does not offer port mapping (UPnP)");
    else if (add) logf_(CFG_LOG_INFO, "game ports: the router passes %d ports on to %s", r, ip_str(ip, a));
    else logf_(CFG_LOG_INFO, "game ports: %d taken back from the router", r);
    _exit(0);
}
static void up_poll(uint32_t now_ms)
{
    uint32_t want = 0; int i;
    if (up_pid > 0) { if (waitpid(up_pid, NULL, WNOHANG) != up_pid) return; up_pid = 0; }
    for (i = 0; i < n_ports && !want; i++)
        if (S[i].cfg.service == CFG_SERVICE_DREAMCAST && S[i].cfg.dc_upnp)
            want = lan_wanted(i) && la[i].ip ? la[i].ip : if_address(lan_if);
    if (want == up_ip && !(want && (up_dirty || (int32_t)(now_ms - up_next_ms) >= 0))) return;
    if (!want && !up_ip) return;
    up_dirty = 0; up_next_ms = now_ms + 1800000u;
    up_pid = fork();
    /* a first or changed address: whatever the router still maps is taken
     * away first, so that nothing stays open towards the old address */
    if (up_pid == 0) upnp_child(!want ? 0 : want != up_ip ? 2 : 1, want);
    if (up_pid < 0) { up_pid = 0; return; }
    up_ip = want;
}
#define VPN_DEV "v90vpn"
static const char *vp_prog = "/usr/sbin/openvpn";      /* -O; "" = none */
static const char *vp_path = "/tmp/v90d.vpn";          /* -V: the profile */
static pid_t    vp_pid; static uint32_t vp_next; static int vp_dirty, vp_stopping, vp_said, vp_on;
static int vp_switch(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (S[i].cfg.service == CFG_SERVICE_DREAMCAST && S[i].cfg.dc_iwango) return 1;
    return 0;
}
static int vp_routes;       /* route lines in the stored profile */
static int vp_remote(char *out, size_t cap)
{
    static char text[VPN_PROFILE_MAX]; int fd = open(vp_path, O_RDONLY | O_CLOEXEC); ssize_t n;
    out[0] = 0;
    if (fd < 0) return 0;
    n = read(fd, text, sizeof text);
    close(fd);
    if (n > 0) (void)vpn_profile_remote(text, (size_t)n, out, cap);
    vp_routes = n > 0 ? vpn_profile_routes(text, (size_t)n) : 0;
    return 1;
}
static int on_vpn(void *ctx, const char *text, size_t len)
{
    char tmp[300]; int fd;
    (void)ctx;
    if (len == 0) {
        if (unlink(vp_path) < 0 && errno != ENOENT) return -1;
        logf_(CFG_LOG_INFO, "IWANGO games: the VPN profile was removed");
    } else {
        snprintf(tmp, sizeof tmp, "%s.new", vp_path);
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0) return -1;
        if (fchmod(fd, 0600) < 0 || write(fd, text, len) != (ssize_t)len) { close(fd); unlink(tmp); return -1; }
        close(fd);
        if (rename(tmp, vp_path) < 0) { unlink(tmp); return -1; }
        logf_(CFG_LOG_INFO, "IWANGO games: a VPN profile was stored");
    }
    vp_dirty = 1;
    return 0;
}
static void vp_poll(uint32_t now_ms)
{
    char remote[48]; int sw = vp_switch(), have = access(vp_path, R_OK) == 0, want = sw && have && vp_prog[0], st;

    if (vp_pid > 0 && waitpid(vp_pid, &st, WNOHANG) == vp_pid) {
        vp_pid = 0;
        if (!vp_stopping) {
            logf_(CFG_LOG_WARN, "IWANGO games: the VPN program ended by itself; again in half a minute");
            vp_next = now_ms + 30000u;
        }
        vp_stopping = 0;
    }
    if (sw && !have) { if (!vp_said) logf_(CFG_LOG_WARN, "IWANGO games: no VPN profile is stored (it is given on the Dreamcast page)"); vp_said = 1; }
    else vp_said = 0;
    if (want != vp_on) { vp_on = want; net_dirty = 1; }
    if (vp_pid > 0) {
        if ((!want || vp_dirty) && !vp_stopping) {
            (void)kill(vp_pid, SIGTERM);
            vp_stopping = 1; vp_next = now_ms;
            if (!want) logf_(CFG_LOG_INFO, "IWANGO games: the VPN is stopped");
        }
        vp_dirty = 0;
        return;
    }
    vp_dirty = 0;
    if (!want || (int32_t)(now_ms - vp_next) < 0) return;
    (void)vp_remote(remote, sizeof remote);
    vp_pid = fork();
    if (vp_pid == 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        (void)sched_setscheduler(0, SCHED_OTHER, &sp);
        (void)munlockall();
        /* after the profile, so that it cannot say otherwise: our device,
         * no scripts, and the box's own traffic and name servers stay */
        /* Of what the server sends, no route is taken: a filter on their
         * text would be a poor guard. The tunnel carries what the profile's
         * own (checked) route lines say, or the games' network as the
         * address list of the console service shows it. */
        if (vp_routes)
            execl(vp_prog, "openvpn", "--config", vp_path, "--dev", VPN_DEV, "--dev-type", "tun", "--script-security", "1",
                  "--pull-filter", "ignore", "redirect-gateway", "--pull-filter", "ignore", "dhcp-option",
                  "--pull-filter", "ignore", "block-outside-dns", "--pull-filter", "ignore", "redirect-private",
                  "--pull-filter", "ignore", "ifconfig-ipv6", "--pull-filter", "accept", "route-gateway",
                  "--pull-filter", "ignore", "route", "--connect-retry", "5", "60", "--verb", "1", (char *)NULL);
        else
            execl(vp_prog, "openvpn", "--config", vp_path, "--dev", VPN_DEV, "--dev-type", "tun", "--script-security", "1",
                  "--pull-filter", "ignore", "redirect-gateway", "--pull-filter", "ignore", "dhcp-option",
                  "--pull-filter", "ignore", "block-outside-dns", "--pull-filter", "ignore", "redirect-private",
                  "--pull-filter", "ignore", "ifconfig-ipv6", "--pull-filter", "accept", "route-gateway",
                  "--pull-filter", "ignore", "route", "--route", "10.200.0.0", "255.255.255.0",
                  "--connect-retry", "5", "60", "--verb", "1", (char *)NULL);
        _exit(127);
    }
    if (vp_pid < 0) { vp_pid = 0; vp_next = now_ms + 30000u; return; }
    logf_(CFG_LOG_INFO, "IWANGO games: starting the VPN (server %s)", remote);
}
static void proxy_arp(int on)
{
    char path[80]; int fd;
    snprintf(path, sizeof path, "/proc/sys/net/ipv4/conf/%s/proxy_arp", lan_if);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) { if (on) logf_(CFG_LOG_WARN, "cannot switch proxy ARP on %s: %s", lan_if, strerror(errno)); return; }
    if (write(fd, on ? "1\n" : "0\n", 2) != 2 && on) logf_(CFG_LOG_WARN, "cannot switch proxy ARP on %s: %s", lan_if, strerror(errno));
    close(fd);
}
static void arch_stop(void)
{
    if (arch_pid > 0) { kill(arch_pid, SIGTERM); (void)waitpid(arch_pid, NULL, 0); arch_pid = 0; }
}
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
static void net_apply(void)
{
    char a[16], b[16];
    int i;
    net_dirty = 0;
    for (i = 0; i < n_ports; i++) {
        cfg_t *c = &S[i].cfg;
        {
            uint32_t want;
            if (!peer_conf[i]) peer_conf[i] = c->ppp_peer;      /* the first time: nothing has replaced it yet */
            want = lan_wanted(i) && la[i].ip ? la[i].ip : peer_conf[i];
            if (want && c->ppp_peer != want) {
                static cfg_t v;
                v = *c; v.ppp_peer = want;
                srv_reconfigure(&S[i], &v);
            }
        }
        if (tun_fd[i] >= 0) tun_close(tun_fd[i]);
        /* an encrypting port carries IP packets 4 octets shorter (the MPPE
         * header and the protocol field ride inside the frame); with the
         * smaller MTU the kernel answers "fragmentation needed" instead of
         * the PPP layer dropping a full-size packet */
        tun_fd[i] = tun_open(tun_name[i], c->ppp_local, c->ppp_peer,
                             c->mru - (c->encryption ? PPP_MPPE_OVERHEAD : 0));
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
        if (vp_on) strcpy(p.vpn, VPN_DEV);
        p.http_port = ARCH_HTTP; p.ftp_port = ARCH_FTP; p.dns_port = ARCH_DNS; p.pasv_lo = ARCH_PASV_LO; p.pasv_hi = ARCH_PASV_HI;
        if (nat_apply(&p) < 0) logf_(CFG_LOG_ERROR, "cannot install the NAT rules: %s", strerror(errno));
        else {
            for (i = 0; i < n_ports; i++) {
                if (p.lan[i]) logf_(CFG_LOG_INFO, "port %d: %s is a host on the network, answered for on %s", i + 1, ip_str(p.peer[i], a), lan_if);
                else if (p.masquerade) logf_(CFG_LOG_INFO, "NAT: %s is masqueraded", ip_str(p.peer[i], a));
            }
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

/* ---- the interface main.c uses (netsvc.h) ---- */
void netsvc_apply(void) { net_apply(); }

void netsvc_poll(uint32_t now_ms)
{
    arch_clock = now_ms;
    if (now_ms % 2000 == 0) arch_poll(now_ms);
    if (now_ms % 1000 == 0) { dc_poll(now_ms); dn_poll(now_ms); la_poll(now_ms); up_poll(now_ms); vp_poll(now_ms); }
}

int  netsvc_on_vpn(void *ctx, const char *profile, size_t len) { return on_vpn(ctx, profile, len); }
int  netsvc_vpn_remote(char *out, size_t cap) { return vp_remote(out, cap); }

void netsvc_stop(void)
{
    arch_stop();
    if (vp_pid > 0) { (void)kill(vp_pid, SIGTERM); (void)waitpid(vp_pid, NULL, 0); }
    if (S[0].cfg.nat || any_archive() || any_console()) (void)nat_remove();
    if (any_lan()) proxy_arp(0);
    if (up_ip) {                        /* give the ports back; the child finishes by itself */
        if (up_pid > 0) (void)waitpid(up_pid, NULL, 0);
        if (fork() == 0) upnp_child(0, 0);
    }
}

void netsvc_set_arch(const char *path)   { arch_path = path; }
void netsvc_set_dclist(const char *cmd)  { dc_cmd = cmd; }
void netsvc_set_dcnow(const char *prog)  { dn_prog = prog; }
void netsvc_set_lease(const char *cmd)   { la_cmd = cmd; }
void netsvc_set_vpnprog(const char *prog){ vp_prog = prog; }
void netsvc_set_vpnpath(const char *path){ vp_path = path; }

uint32_t netsvc_lan_ip(int port) { return (port >= 0 && port < PORTS) ? la[port].ip : 0; }
void     netsvc_set_peer_conf(int port, uint32_t ppp_peer) { if (port >= 0 && port < PORTS) peer_conf[port] = ppp_peer; }
