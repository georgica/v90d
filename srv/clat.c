/* The callers' IPv4 over IPv6 on the box (clat.h). Ours; written for this project. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "daemon.h"
#include "clat.h"
#include "nat.h"
#include "ra6.h"
#include "tun.h"
#include "bbsdial.h"

#define ROUTE_MS 5000           /* the IPv4 default route looked for this often */
#define RETRY_MS 30000          /* a failed start tried again after */

/* the callers' /96: a unique local prefix of the box's own (fd for local,
 * then "90d" and "c1a7"); nothing outside the box sees it */
static const uint8_t CLAT96[16] = { 0xfd, 0x00, 0x09, 0x0d, 0xc1, 0xa7, 0, 0, 0, 0, 0, 0 };

static struct {
    int want, up, fd, v4;
    char lan[16];
    uint32_t next_route, next_try, now_s;
    uint8_t pref64[16]; int pref64_len;
    xlat_t x;
    uint8_t out[1600];
} C = { .fd = -1, .v4 = 1 };

xlat_t *clat_xlat(void) { return C.up ? &C.x : NULL; }

uint32_t clat_standin(const uint8_t v6[16])
{
    if (!C.up || bbsdial_is_self6(v6) || !bbsdial_is_public6(v6)) return 0;
    return xlat_pool_get(&C.x, v6, C.now_s);
}
int clat_up(void) { return C.up; }
int clat_v4_route(void) { return C.v4; }

static void put(const char *path, const char *v)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(v, f); fclose(f); }
}

/* does the box have an IPv4 default route? */
static int v4_route(void)
{
    FILE *f = fopen("/proc/net/route", "r");
    char l[256];
    int yes = 0;
    if (!f) return 1;                           /* cannot tell: as if it had */
    while (fgets(l, sizeof l, f)) {
        char ifn[32]; unsigned long dst, gw, mask; unsigned flags;
        if (sscanf(l, "%31s %lx %lx %x %*d %*d %*d %lx", ifn, &dst, &gw, &flags, &mask) == 5
            && dst == 0 && mask == 0 && (flags & 1)) { yes = 1; break; }
    }
    fclose(f);
    return yes;
}

static void down(void)
{
    if (C.fd >= 0) { tun_close(C.fd); C.fd = -1; }
    if (C.up) {
        (void)nat_clat6(0, CLAT96);
        logf_(CFG_LOG_INFO, "ipv6: the callers' translator is down");
    }
    C.up = 0;
}

void clat_want(int on, const char *lan_if)
{
    C.want = on && lan_if && *lan_if;
    if (C.want) snprintf(C.lan, sizeof C.lan, "%s", lan_if);
    if (!C.want) down();
}

static void bring_up(uint32_t now)
{
    char p[96];
    C.next_try = now + RETRY_MS;
    /* forwarding stops the kernel taking advertisements unless accept_ra is 2 */
    snprintf(p, sizeof p, "/proc/sys/net/ipv6/conf/%s/accept_ra", C.lan);
    put(p, "2");
    C.fd = tun_open6(CLAT_IF, 1500, CLAT96, 96);
    if (C.fd < 0) { logf_(CFG_LOG_WARN, "ipv6: no translator for the callers: %s %s", CLAT_IF, strerror(errno)); return; }
    if (nat_clat6(1, CLAT96) < 0) {
        logf_(CFG_LOG_WARN, "ipv6: no translator for the callers: the IPv6 masquerade: %s", strerror(errno));
        tun_close(C.fd); C.fd = -1;
        return;
    }
    put("/proc/sys/net/ipv6/conf/all/forwarding", "1");
    xlat_init(&C.x, CLAT96);
    C.up = 1;
}

void clat_poll(uint32_t now)
{
    const ra6_info *r = ra6_now();
    int i;
    if (!C.want) return;
    C.now_s = now / 1000;
    if ((int32_t)(now - C.next_route) >= 0) {
        int v4 = v4_route();
        C.next_route = now + ROUTE_MS;
        if (v4 != C.v4 && C.up) logf_(CFG_LOG_INFO, "ipv6: %s", v4 ? "the box has IPv4 out again: the callers' IPv4 goes as IPv4" :
                                                         "no IPv4 out: the callers' IPv4 goes through NAT64");
        C.v4 = v4;
    }
    /* the NAT64 prefix as the router says */
    if (r->pref64_len != C.pref64_len || memcmp(r->pref64, C.pref64, 16) != 0) {
        memcpy(C.pref64, r->pref64, 16); C.pref64_len = r->pref64_len;
        if (C.up) xlat_set_pref64(&C.x, C.pref64, C.pref64_len);
    }
    if (!C.up && C.pref64_len && (int32_t)(now - C.next_try) >= 0) {
        bring_up(now);
        if (C.up) {
            char a[INET6_ADDRSTRLEN];
            xlat_set_pref64(&C.x, C.pref64, C.pref64_len);
            inet_ntop(AF_INET6, C.pref64, a, sizeof a);
            logf_(CFG_LOG_INFO, "ipv6: the callers' translator is up (NAT64 %s/%d)%s", a, C.pref64_len,
                  C.v4 ? "; the box has IPv4 out, so only stand-ins go through it" : "; no IPv4 out: the callers' IPv4 goes through it");
        }
    }
    if (!C.up) return;
    /* what comes back, for the callers */
    for (i = 0; i < 16; i++) {
        uint8_t in[1600];
        size_t n;
        uint32_t dst;
        int p;
        ssize_t r6 = tun_read(C.fd, in, sizeof in);
        if (r6 <= 0) break;
        if (xlat_6to4(&C.x, in, (size_t)r6, C.out, sizeof C.out, &n, now / 1000) != XLAT_OUT) continue;
        dst = (uint32_t)C.out[16] << 24 | (uint32_t)C.out[17] << 16 | (uint32_t)C.out[18] << 8 | C.out[19];
        for (p = 0; p < n_ports; p++)
            if (S[p].in_call && S[p].cfg.ppp_peer == dst) { (void)srv_send_ip(&S[p], C.out, n); break; }
    }
}

int clat_from_caller(int p, const uint8_t *pkt, size_t len)
{
    uint32_t dst;
    size_t n;
    int r;
    if (!C.up || len < 20 || (pkt[0] >> 4) != 4) return 0;
    /* only the port's own caller address: no port puts another's source on
     * the translator (its return map keys on the caller address) */
    if (((uint32_t)pkt[12] << 24 | (uint32_t)pkt[13] << 16 | (uint32_t)pkt[14] << 8 | pkt[15]) != S[p].cfg.ppp_peer) return 0;
    dst = (uint32_t)pkt[16] << 24 | (uint32_t)pkt[17] << 16 | (uint32_t)pkt[18] << 8 | pkt[19];
    if (!xlat_is_pool(dst)) {
        if (C.v4) return 0;                                 /* the box has IPv4 out: as IPv4 */
        if (!bbsdial_is_public(dst)) return 0;              /* the box itself, the LAN: as IPv4, never NAT64 */
    }
    r = xlat_4to6(&C.x, pkt, len, C.out, sizeof C.out, &n, C.now_s);
    if (r == XLAT_OUT) { if (tun_write(C.fd, C.out, n) < 0) S[p].status.rx_errors++; }
    else if (r == XLAT_BACK) (void)srv_send_ip(&S[p], C.out, n);
    return 1;
}
