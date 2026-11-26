/* See webtv.h. */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "webtv.h"

int webtv_public(uint32_t a)
{
    static const struct { uint32_t net, mask; } no[] = {
        { 0x00000000u, 0xFF000000u },   /* 0/8 */
        { 0x0A000000u, 0xFF000000u },   /* 10/8 */
        { 0x64400000u, 0xFFC00000u },   /* 100.64/10 */
        { 0x7F000000u, 0xFF000000u },   /* 127/8 */
        { 0xA9FE0000u, 0xFFFF0000u },   /* 169.254/16 */
        { 0xAC100000u, 0xFFF00000u },   /* 172.16/12 */
        { 0xC0000000u, 0xFFFFFF00u },   /* 192.0.0/24 */
        { 0xC0000200u, 0xFFFFFF00u },   /* 192.0.2/24 */
        { 0xC0A80000u, 0xFFFF0000u },   /* 192.168/16 */
        { 0xC6120000u, 0xFFFE0000u },   /* 198.18/15 */
        { 0xC6336400u, 0xFFFFFF00u },   /* 198.51.100/24 */
        { 0xCB007100u, 0xFFFFFF00u },   /* 203.0.113/24 */
        { 0xE0000000u, 0xE0000000u },   /* 224/3: multicast and reserved */
    };
    unsigned i;
    for (i = 0; i < sizeof no / sizeof no[0]; i++) if ((a & no[i].mask) == no[i].net) return 0;
    return 1;
}

uint32_t webtv_pick(const struct addrinfo *ai)
{
    for (; ai; ai = ai->ai_next) {
        const struct sockaddr_in *s;
        uint32_t a;
        if (ai->ai_family != AF_INET || !ai->ai_addr || ai->ai_addrlen < sizeof *s) continue;
        s = (const struct sockaddr_in *)(const void *)ai->ai_addr;
        a = ntohl(s->sin_addr.s_addr);
        if (webtv_public(a)) return a;
    }
    return 0;
}
