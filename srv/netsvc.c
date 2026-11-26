/* See netsvc.h. The daemon's network layer (taken out of srv/main.c in
 * 0.2.3, PROTOWEB's per-call NAT since): the per-port services' background work (the address
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
#include <netdb.h>
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
#include "webtv.h"

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
/* PROTOWEB (srv.h, SRV_PROTOWEB): a call to that number has the archive
 * for itself, on a port whose service is the plain internet. The NAT plan
 * then sends that port's web, FTP and names to the gateway while the call
 * lasts; a child process installs it (pw_poll), so the line's loop never
 * waits on it. The gateway, once started for such a call, keeps running. */
static int   pw_used;                   /* a PROTOWEB call has been: the gateway stays */
static int   arch_on[PORTS];            /* what the NAT plan in force gives each port */
static pid_t natr_pid; static int natr_again;
static int port_archive(int i) { return S[i].cfg.service == CFG_SERVICE_ARCHIVE || srv_protoweb(&S[i]); }
/* WebTV (srv.h, srv_webtv; webtv.h): a call on a WebTV port has 10.0.0.1
 * sent to WebTV Redialed and the box's fixed name server to the port's,
 * while it lasts (pw_poll, like PROTOWEB's). The address of WebTV
 * Redialed is looked up by a child (wt_poll) every 10 minutes while a port
 * is a WebTV or a Dreamcast port (a Dreamcast's 10.0.1.129 goes there too,
 * for as long as the port has that service); a lookup that fails keeps the
 * last good address.
 * -Q gives one instead (no lookup), -Q "" neither. */
static uint32_t wt_addr;                /* WebTV Redialed, host order; 0 = not known */
static const char *wt_fixed;            /* -Q */
static pid_t    wt_pid; static int wt_fd = -1; static uint32_t wt_next_ms, wt_kill_ms; static int wt_started, wt_failed;
static uint8_t  wt_got[4]; static size_t wt_len;
static int      wt_on[PORTS];           /* what the NAT plan in force gives each port */
static int any_webtv(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (srv_webtv(&S[i])) return 1;
    return 0;
}
static int any_archive(void)
{
    int i;
    for (i = 0; i < n_ports; i++) if (port_archive(i)) return 1;
    return pw_used;
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
/* The address list of the game revival projects (dclist.h): fetched by a
 * child process while a port has the console service. The child writes the
 * list to its standard output, a pipe we read as it comes (no file, so
 * nothing in /tmp to be put in its way); it is parsed when the child is
 * done and in force from the next time the line is idle. A fetch that fails
 * leaves the list as it was. */
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
            (void)fcntl(pp[1], F_SETFL, 0);                 /* the child writes without hurry */
            child_plain(-1, pp[1], 0);
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
/* "Dreamcast Now" (dcnow.h): while the caller of a console port that asks
 * for it has its network up, a report every 15 s, sent by a child process
 * (the image's wget; dn_prog, when set, is run instead as "prog URL BODY"). */
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
                child_plain(-1, -1, 0);
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
/* A console as a host on the network (dreamcast_lan): the caller of that
 * port gets an address of the network itself. The address comes from a
 * command that prints it (on the box, udhcpc asking the network's DHCP
 * server for a second lease under a client name of its own; the script it
 * is given prints the address and changes nothing). It is asked again every
 * half hour, which also keeps the lease. Without an answer the port stays
 * as the settings have it, behind this box's address. */
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
                (void)fcntl(pp[1], F_SETFL, 0);
                child_plain(-1, pp[1], 0);
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
    const nat_open *t; int n = dclist_ports(&t), i, r; char a[16];
    /* this child does not become another program: it must let go of the
     * daemon's lines, network devices and listening sockets itself */
    child_plain(-1, -1, 1);
    for (i = 0; i < n; i++) { proto[i] = t[i].proto; lo[i] = t[i].lo; hi[i] = t[i].hi; }
    r = upnp_run(if_address(lan_if), add, ip, proto, lo, hi, n, "v90d: Dreamcast");
    if (r == -1) logf_(CFG_LOG_WARN, "game ports: no router answered the search (UPnP)");
    else if (r == -2) logf_(CFG_LOG_WARN, "game ports: the router does not offer port mapping (UPnP)");
    else if (add) logf_(CFG_LOG_INFO, "game ports: the router passes %d ports on to %s", r, ip_str(ip, a));
    else logf_(CFG_LOG_INFO, "game ports: %d taken back from the router", r);
    _exit(0);
}
/* Game ports from the router (dreamcast_upnp): a child process asks the
 * network's router to pass the ports of dclist.h on, to the console itself
 * when it is a host on the network and to this box otherwise (which passes
 * them on). Asked again whenever the addresses change and every half hour;
 * taken back when the switch goes off and when the daemon stops. */
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
/* ---- the VPN for the IWANGO games (dreamcast_iwango). The games' servers
 * of today stand in a private network behind an OpenVPN server; the owner
 * of the box has a client profile for it and gives it to the Dreamcast
 * page (cfg/vpn.h checks it). OpenVPN itself is not ours: the image carries
 * the program as it is (DESIGN.md, the owner's decision), and this code
 * only keeps it running while a dreamcast port wants it. ---- */
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
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
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
        child_plain(-1, -1, 0);
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
/* Stop the gateway. wait: also wait for it (the daemon's own end); in the
 * loop it is only told, and arch_poll reaps it without waiting. */
static int arch_told;
static void arch_stop(int wait)
{
    if (arch_pid <= 0) return;
    if (!arch_told) { kill(arch_pid, SIGTERM); arch_told = 1; }
    if (wait) { (void)waitpid(arch_pid, NULL, 0); arch_pid = 0; arch_told = 0; }
}
static void arch_poll(uint32_t now_ms)
{
    if (arch_pid > 0 && waitpid(arch_pid, NULL, WNOHANG) == arch_pid) {
        arch_pid = 0;
        if (arch_want && !arch_told) logf_(CFG_LOG_WARN, "archive mode: the gateway stopped; starting it again");
        arch_told = 0;
    }
    if (!arch_want) { arch_stop(0); return; }
    if (arch_told) return;                  /* the one told to stop is still going */
    if (arch_pid > 0 || !arch_path[0] || (int32_t)(now_ms - arch_next_ms) < 0) return;
    arch_next_ms = now_ms + 5000;
    arch_pid = fork();
    if (arch_pid == 0) {
        /* the child is an ordinary process: no real-time priority, no locked memory */
        child_plain(-1, -1, 0);
        /* every name the gateway gives out is in this block: what is not
         * redirected to it (a secure site, mail) is refused at once instead
         * of waiting for an answer that cannot come */
        if (system("ip route replace unreachable 198.18.0.0/15 2>/dev/null") != 0) { /* the gateway works without it */ }
        execl(arch_path, arch_path, "-r", "-w", STR(ARCH_HTTP), "-f", STR(ARCH_FTP), "-d", STR(ARCH_DNS),
              "-p", STR(ARCH_PASV_LO) "-" STR(ARCH_PASV_HI), (char *)NULL);
        _exit(127);
    }
    if (arch_pid < 0) { arch_pid = 0; logf_(CFG_LOG_ERROR, "archive mode: cannot start %s: %s", arch_path, strerror(errno)); }
    else logf_(CFG_LOG_INFO, "archive mode: %s started", arch_path);
}
static uint32_t arch_clock;
static void arch_apply(int on) { arch_want = on; arch_next_ms = arch_clock; arch_poll(arch_clock); }
/* the NAT plan for the settings and the calls now; 0 = no rules at all */
static int build_plan(nat_plan *p)
{
    int i;
    memset(p, 0, sizeof *p);
    if (!(S[0].cfg.nat || any_archive() || any_console() || any_webtv())) return 0;
    p->peer[0] = S[0].cfg.ppp_peer; p->peer[1] = n_ports > 1 ? S[1].cfg.ppp_peer : 0;
    p->masquerade = S[0].cfg.nat;
    for (i = 0; i < n_ports; i++) p->archive[i] = port_archive(i);
    for (i = 0; i < n_ports; i++) p->console[i] = S[i].cfg.service == CFG_SERVICE_DREAMCAST;
    for (i = 0; i < n_ports; i++) p->lan[i] = lan_wanted(i) && la[i].ip;
    for (i = n_ports - 1; i >= 0; i--) {
        /* a WebTV box: its names to the server the port gives its callers;
         * a Dreamcast port: its one address of the old network (nat.h) */
        if (srv_webtv(&S[i])) {
            p->webtv[i] = NAT_WEBTV_BOX;
            p->webtv_dns = S[i].cfg.dns_relay ? S[i].cfg.ppp_local : S[i].cfg.dns[0];
        } else if (S[i].cfg.service == CFG_SERVICE_DREAMCAST) {
            p->webtv[i] = NAT_WEBTV_DC;
        }
    }
    p->webtv_to = wt_addr;
    p->rewrite = dc_rw; p->n_rewrite = dc_n;
    p->n_open = dclist_ports(&p->open);
    strncpy(p->wan, lan_if, sizeof p->wan - 1);
    if (vp_on) strcpy(p->vpn, VPN_DEV);
    p->http_port = ARCH_HTTP; p->ftp_port = ARCH_FTP; p->dns_port = ARCH_DNS; p->pasv_lo = ARCH_PASV_LO; p->pasv_hi = ARCH_PASV_HI;
    return 1;
}

/* the plan again, by a child (a call began or ended: the loop must not wait) */
static void nat_refresh(void)
{
    nat_plan p; int need;
    if (natr_pid > 0) { natr_again = 1; return; }
    natr_again = 0;
    need = build_plan(&p);
    natr_pid = fork();
    if (natr_pid == 0) {
        child_plain(-1, -1, 0);
        if (need) _exit(nat_apply(&p) < 0);
        _exit(nat_remove() < 0 && errno != ENOENT);
    }
    if (natr_pid < 0) { natr_pid = 0; logf_(CFG_LOG_ERROR, "cannot change the NAT rules: %s", strerror(errno)); }
}

static void nat_refresh(void);
static void wt_drain(void)
{
    ssize_t r;
    while (wt_fd >= 0 && wt_len < sizeof wt_got && (r = read(wt_fd, wt_got + wt_len, sizeof wt_got - wt_len)) > 0) wt_len += (size_t)r;
}
static void wt_poll(uint32_t now_ms)
{
    int i, st, can = 0;
    char a[16];
    if (wt_pid > 0) {
        wt_drain();
        if (waitpid(wt_pid, &st, WNOHANG) != wt_pid) {
            if ((int32_t)(now_ms - wt_kill_ms) >= 0) (void)kill(wt_pid, SIGKILL);   /* a lookup that hangs */
            return;
        }
        wt_drain();
        wt_pid = 0; close(wt_fd); wt_fd = -1;
        wt_next_ms = now_ms + WEBTV_EVERY_MS;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && wt_len == 4) {
            uint32_t got = ((uint32_t)wt_got[0] << 24) | ((uint32_t)wt_got[1] << 16) | ((uint32_t)wt_got[2] << 8) | wt_got[3];
            wt_failed = 0;
            if (got && got != wt_addr) {
                struct in_addr in; in.s_addr = htonl(got);
                wt_addr = got;
                logf_(CFG_LOG_INFO, "WebTV Redialed (%s) is at %s", WEBTV_HOST, inet_ntop(AF_INET, &in, a, sizeof a) ? a : "?");
                if (any_webtv()) nat_refresh();
                if (any_console()) net_dirty = 1;   /* the Dreamcast's 10.0.1.129: when the line is idle */
            }
        } else if (!wt_failed++) {
            logf_(CFG_LOG_WARN, "WebTV Redialed (%s) could not be looked up; %s", WEBTV_HOST,
                  wt_addr ? "keeping the address it had" : "WebTV calls get no service until it can");
        }
        return;
    }
    if (wt_fixed) return;
    for (i = 0; i < n_ports; i++)
        if (S[i].cfg.service == CFG_SERVICE_WEBTV || S[i].cfg.service == CFG_SERVICE_DREAMCAST) can = 1;
    if (!can) return;
    if (wt_started && (int32_t)(now_ms - wt_next_ms) < 0) return;
    wt_started = 1; wt_next_ms = now_ms + WEBTV_EVERY_MS;
    {
        int pp[2];
        if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) return;
        wt_len = 0;
        wt_pid = fork();
        if (wt_pid == 0) {
            struct addrinfo hints, *res = NULL; uint32_t x = 0; uint8_t b[4];
            /* the pipe becomes its standard output and every other
             * descriptor is closed: this child runs no program, so nothing
             * else would close the daemon's TUN devices and sockets, and a
             * TUN device still open here cannot be set up again (EBUSY) */
            (void)fcntl(pp[1], F_SETFL, 0);
            child_plain(-1, pp[1], 1);
            memset(&hints, 0, sizeof hints);
            hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(WEBTV_HOST, NULL, &hints, &res) == 0) { x = webtv_pick(res); freeaddrinfo(res); }
            if (!x) _exit(1);
            b[0] = (uint8_t)(x >> 24); b[1] = (uint8_t)(x >> 16); b[2] = (uint8_t)(x >> 8); b[3] = (uint8_t)x;
            _exit(write(1, b, 4) == 4 ? 0 : 1);
        }
        close(pp[1]);
        if (wt_pid < 0) { wt_pid = 0; close(pp[0]); return; }
        wt_fd = pp[0]; wt_kill_ms = now_ms + 60000u;
    }
}

static void pw_poll(void)
{
    int i, changed = 0, st;
    if (natr_pid > 0 && waitpid(natr_pid, &st, WNOHANG) == natr_pid) {
        natr_pid = 0;
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) logf_(CFG_LOG_ERROR, "cannot change the NAT rules for a PROTOWEB call");
        if (natr_again) nat_refresh();
    }
    for (i = 0; i < n_ports; i++) {
        int w = port_archive(i);
        if (w == arch_on[i]) continue;
        arch_on[i] = w; changed = 1;
        if (srv_protoweb(&S[i])) {
            pw_used = 1;
            logf_(CFG_LOG_INFO, "port %d: a call to PROTOWEB: its web, FTP and names go to the archive gateway", i + 1);
        }
    }
    for (i = 0; i < n_ports; i++) {
        int w = srv_webtv(&S[i]);
        if (w == wt_on[i]) continue;
        wt_on[i] = w; changed = 1;
        if (w && wt_addr) {
            struct in_addr in; char a[16]; in.s_addr = htonl(wt_addr);
            logf_(CFG_LOG_INFO, "port %d: a call to WebTV: 10.0.0.1 goes to WebTV Redialed (%s)", i + 1,
                  inet_ntop(AF_INET, &in, a, sizeof a) ? a : "?");
        } else if (w) {
            logf_(CFG_LOG_WARN, "port %d: a call to WebTV, but WebTV Redialed's address is not known yet", i + 1);
        }
    }
    if (changed) { nat_refresh(); arch_apply(any_archive()); }
}

static void net_apply(void)
{
    char a[16], b[16];
    nat_plan p;
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
    if (natr_pid > 0) { (void)waitpid(natr_pid, NULL, 0); natr_pid = 0; natr_again = 0; }
    for (i = 0; i < n_ports; i++) arch_on[i] = port_archive(i);
    if (build_plan(&p)) {
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
    /* Deadlines, not "now_ms % N == 0": now_ms wraps after 49.7 days and
     * 2^32 is no multiple of the 10 ms step, so after the wrap no value of
     * it is a multiple of 100 again for months, and nothing here would run. */
    static uint32_t at100, at1000, at2000;
    if ((int32_t)(now_ms - at100) >= 0) { at100 = now_ms + 100; pw_poll(); }
    if ((int32_t)(now_ms - at1000) >= 0) wt_poll(now_ms);
    if ((int32_t)(now_ms - at2000) >= 0) { at2000 = now_ms + 2000; arch_poll(now_ms); }
    if ((int32_t)(now_ms - at1000) >= 0) {
        at1000 = now_ms + 1000;
        dc_poll(now_ms); dn_poll(now_ms); la_poll(now_ms); up_poll(now_ms); vp_poll(now_ms);
    }
}

int  netsvc_on_vpn(void *ctx, const char *profile, size_t len) { return on_vpn(ctx, profile, len); }
int  netsvc_vpn_remote(char *out, size_t cap) { return vp_remote(out, cap); }

void netsvc_stop(void)
{
    arch_stop(1);
    if (natr_pid > 0) { (void)waitpid(natr_pid, NULL, 0); natr_pid = 0; }
    if (vp_pid > 0) { (void)kill(vp_pid, SIGTERM); (void)waitpid(vp_pid, NULL, 0); }
    if (wt_pid > 0) { (void)kill(wt_pid, SIGKILL); (void)waitpid(wt_pid, NULL, 0); wt_pid = 0; }
    if (S[0].cfg.nat || any_archive() || any_console() || any_webtv()) (void)nat_remove();
    if (any_lan()) proxy_arp(0);
    if (up_ip) {                        /* give the ports back; the child finishes by itself */
        if (up_pid > 0) (void)waitpid(up_pid, NULL, 0);
        if (fork() == 0) upnp_child(0, 0);
    }
}

void netsvc_set_arch(const char *path)   { arch_path = path; }
int  netsvc_has_arch(void) { return arch_path[0] && access(arch_path, X_OK) == 0; }
void netsvc_set_dclist(const char *cmd)  { dc_cmd = cmd; }
void netsvc_set_dcnow(const char *prog)  { dn_prog = prog; }
void netsvc_set_webtv(const char *addr)
{
    struct in_addr in;
    wt_fixed = addr;
    wt_addr = addr[0] && inet_pton(AF_INET, addr, &in) == 1 ? ntohl(in.s_addr) : 0;
}
void netsvc_set_lease(const char *cmd)   { la_cmd = cmd; }
void netsvc_set_vpnprog(const char *prog){ vp_prog = prog; }
void netsvc_set_vpnpath(const char *path){ vp_path = path; }

uint32_t netsvc_lan_ip(int port) { return (port >= 0 && port < PORTS) ? la[port].ip : 0; }
void     netsvc_set_peer_conf(int port, uint32_t ppp_peer) { if (port >= 0 && port < PORTS) peer_conf[port] = ppp_peer; }
