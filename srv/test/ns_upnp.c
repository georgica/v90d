/* The UPnP conversation of upnp.h against a stand-in for a home router, in a
 * network namespace of its own (`unshare -Urn`, see the Makefile). The
 * stand-in (a child process) answers the search on the multicast group,
 * serves a device description, and writes down every mapping request it is
 * sent; one port it refuses, as a router with that port taken would. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <net/route.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "test.h"
#include "upnp.h"

#define LO 0x7F000001u

static const char desc[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nConnection: close\r\n\r\n"
    "<?xml version=\"1.0\"?><root><device><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
    "<controlURL>/ctl/Cmn</controlURL></service></serviceList><deviceList><device><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
    "<controlURL>/ctl/IPConn</controlURL></service></serviceList></device></deviceList></device></root>";

static const char *between(const char *s, const char *a, const char *b, char *out, size_t cap)
{
    const char *p = strstr(s, a), *q;
    out[0] = 0;
    if (!p) return out;
    p += strlen(a); q = strstr(p, b);
    if (!q || (size_t)(q - p) >= cap) return out;
    memcpy(out, p, (size_t)(q - p)); out[q - p] = 0;
    return out;
}

static void router(const char *logpath)
{
    int u = socket(AF_INET, SOCK_DGRAM, 0), t = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in sa; struct ip_mreq mr; fd_set fds;
    static char req[4096];

    setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(t, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(1900);
    if (bind(u, (struct sockaddr *)(void *)&sa, sizeof sa) < 0) _exit(3);
    mr.imr_multiaddr.s_addr = htonl(UPNP_GROUP); mr.imr_interface.s_addr = htonl(LO);
    if (setsockopt(u, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof mr) < 0) _exit(4);
    sa.sin_port = htons(5000);
    if (bind(t, (struct sockaddr *)(void *)&sa, sizeof sa) < 0 || listen(t, 8) < 0) _exit(5);
    for (;;) {
        FD_ZERO(&fds); FD_SET(u, &fds); FD_SET(t, &fds);
        if (select((u > t ? u : t) + 1, &fds, NULL, NULL, NULL) <= 0) continue;
        if (FD_ISSET(u, &fds)) {
            struct sockaddr_in from; socklen_t fl = sizeof from;
            ssize_t r = recvfrom(u, req, sizeof req - 1, 0, (struct sockaddr *)(void *)&from, &fl);
            if (r > 0) {
                req[r] = 0;
                if (strstr(req, "M-SEARCH") && strstr(req, "InternetGatewayDevice:1")) {
                    static const char ans[] = "HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                              "LOCATION: http://127.0.0.1:5000/desc.xml\r\n\r\n";
                    sendto(u, ans, sizeof ans - 1, 0, (struct sockaddr *)(void *)&from, fl);
                }
            }
        }
        if (FD_ISSET(t, &fds)) {
            int c = accept(t, NULL, NULL); size_t got = 0; ssize_t r;
            if (c < 0) continue;
            /* headers, then as much body as Content-Length says */
            while (got < sizeof req - 1 && (r = recv(c, req + got, sizeof req - 1 - got, 0)) > 0) {
                char *e, *cl; got += (size_t)r; req[got] = 0;
                e = strstr(req, "\r\n\r\n");
                if (!e) continue;
                cl = strstr(req, "Content-Length: ");
                if (!cl || got >= (size_t)(e + 4 - req) + (size_t)atoi(cl + 16)) break;
            }
            req[got] = 0;
            if (!strncmp(req, "GET /desc.xml ", 14)) {
                send(c, desc, sizeof desc - 1, MSG_NOSIGNAL);
            } else if (!strncmp(req, "POST /ctl/IPConn ", 17)) {
                char port[16], proto[8], client[24], what[40]; FILE *f;
                int add = strstr(req, "#AddPortMapping\"") != NULL, del = strstr(req, "#DeletePortMapping\"") != NULL;
                between(req, "<NewExternalPort>", "</NewExternalPort>", port, sizeof port);
                between(req, "<NewProtocol>", "</NewProtocol>", proto, sizeof proto);
                between(req, "<NewInternalClient>", "</NewInternalClient>", client, sizeof client);
                between(req, "<NewPortMappingDescription>", "</NewPortMappingDescription>", what, sizeof what);
                f = fopen(logpath, "a");
                if (f) { fprintf(f, "%s %s %s %s [%s]\n", add ? "add" : del ? "del" : "?", proto, port, client, what); fclose(f); }
                if (add && !strcmp(port, "2350")) send(c, "HTTP/1.1 500 Internal Server Error\r\n\r\n", 38, MSG_NOSIGNAL);
                else send(c, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 38, MSG_NOSIGNAL);
            } else {
                send(c, "HTTP/1.1 404 Not Found\r\n\r\n", 26, MSG_NOSIGNAL);
            }
            close(c);
        }
    }
}

static int file_has(const char *path, const char *needle)
{
    static char b[8192]; FILE *f = fopen(path, "r"); size_t n;
    if (!f) return 0;
    n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return strstr(b, needle) != NULL;
}

int main(int argc, char **argv)
{
    static const uint8_t proto[2] = { 17, 6 };
    static const uint16_t lo[2] = { 7980, 2349 }, hi[2] = { 7980, 2351 };
    char logpath[300]; struct ifreq ifr; struct rtentry rt; struct sockaddr_in *sa; int sk; pid_t pid;

    snprintf(logpath, sizeof logpath, "%s/ns_upnp.log", argc > 1 ? argv[1] : ".");
    unlink(logpath);
    /* the loopback device up, able to carry the multicast group */
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "lo");
    if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0) { printf("ns_upnp: no network of its own here: not tried\n"); return 0; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_MULTICAST;
    CHECK_EQ_I(ioctl(sk, SIOCSIFFLAGS, &ifr), 0);
    memset(&rt, 0, sizeof rt);
    sa = (struct sockaddr_in *)(void *)&rt.rt_dst; sa->sin_family = AF_INET; sa->sin_addr.s_addr = htonl(0xE0000000u);
    sa = (struct sockaddr_in *)(void *)&rt.rt_genmask; sa->sin_family = AF_INET; sa->sin_addr.s_addr = htonl(0xF0000000u);
    rt.rt_flags = RTF_UP; rt.rt_dev = (char *)"lo";
    CHECK_EQ_I(ioctl(sk, SIOCADDRT, &rt), 0);
    close(sk);

    /* nobody there: no router */
    CHECK_EQ_I(upnp_run(LO, 1, 0xC000024Du, proto, lo, hi, 2, "v90d: Dreamcast"), -1);

    pid = fork();
    if (pid == 0) { router(logpath); _exit(0); }
    usleep(200000);

    /* four ports asked for, one of them refused */
    CHECK_EQ_I(upnp_run(LO, 1, 0xC000024Du, proto, lo, hi, 2, "v90d: Dreamcast"), 3);
    CHECK(file_has(logpath, "add UDP 7980 192.0.2.77 [v90d: Dreamcast]\n"));
    CHECK(file_has(logpath, "add TCP 2349 192.0.2.77 [v90d: Dreamcast]\n"));
    CHECK(file_has(logpath, "add TCP 2350 192.0.2.77 [v90d: Dreamcast]\n"));
    CHECK(file_has(logpath, "add TCP 2351 192.0.2.77 [v90d: Dreamcast]\n"));
    CHECK(!file_has(logpath, "del "));
    /* and taken away again */
    CHECK_EQ_I(upnp_run(LO, 0, 0, proto, lo, hi, 2, NULL), 4);
    CHECK(file_has(logpath, "del UDP 7980  []\n"));
    CHECK(file_has(logpath, "del TCP 2351  []\n"));

    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    T_REPORT("ns_upnp");
}
