/* srv/webtv.c: which answer of the lookup of WebTV Redialed's name is taken */
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "test.h"
#include "webtv.h"

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static struct sockaddr_in sa[6];
static struct addrinfo ai[6];
/* a chain of n answers, the addresses in host order */
static const struct addrinfo *chain(const uint32_t *a, int n)
{
    int i;
    memset(sa, 0, sizeof sa); memset(ai, 0, sizeof ai);
    for (i = 0; i < n; i++) {
        sa[i].sin_family = AF_INET; sa[i].sin_addr.s_addr = htonl(a[i]);
        ai[i].ai_family = AF_INET; ai[i].ai_addr = (struct sockaddr *)(void *)&sa[i];
        ai[i].ai_addrlen = sizeof sa[i];
        ai[i].ai_next = i + 1 < n ? &ai[i + 1] : NULL;
    }
    return n ? &ai[0] : NULL;
}

int main(void)
{
    static const uint32_t one[] = { IP4(31, 97, 100, 150) };
    static const uint32_t mixed[] = { IP4(10, 1, 2, 3), IP4(127, 0, 0, 1), IP4(192, 168, 1, 9),
                                      IP4(100, 64, 0, 1), IP4(31, 97, 100, 150), IP4(8, 8, 8, 8) };
    static const uint32_t none[] = { IP4(10, 0, 0, 1), IP4(172, 16, 5, 5), IP4(169, 254, 1, 1), IP4(0, 0, 0, 0),
                                     IP4(224, 0, 0, 1), IP4(255, 255, 255, 255) };
    struct sockaddr_in6 s6; struct addrinfo a6;

    CHECK(strcmp(WEBTV_HOST, "community.webtv.zone") == 0);
    CHECK(webtv_pick(chain(one, 1)) == IP4(31, 97, 100, 150));
    CHECK(webtv_pick(chain(mixed, 6)) == IP4(31, 97, 100, 150));      /* the first public one */
    CHECK(webtv_pick(chain(none, 6)) == 0);
    CHECK(webtv_pick(NULL) == 0);
    /* an IPv6 answer is passed over */
    memset(&s6, 0, sizeof s6); memset(&a6, 0, sizeof a6);
    s6.sin6_family = AF_INET6; a6.ai_family = AF_INET6;
    a6.ai_addr = (struct sockaddr *)(void *)&s6; a6.ai_addrlen = sizeof s6;
    a6.ai_next = (struct addrinfo *)chain(one, 1);
    CHECK(webtv_pick(&a6) == IP4(31, 97, 100, 150));
    a6.ai_next = NULL;
    CHECK(webtv_pick(&a6) == 0);
    /* the private and special blocks one by one */
    CHECK(!webtv_public(IP4(10, 255, 0, 1)) && !webtv_public(IP4(100, 127, 255, 255)) && webtv_public(IP4(100, 128, 0, 1)));
    CHECK(!webtv_public(IP4(172, 31, 0, 1)) && webtv_public(IP4(172, 32, 0, 1)) && !webtv_public(IP4(192, 0, 2, 7)));
    CHECK(!webtv_public(IP4(198, 18, 0, 1)) && !webtv_public(IP4(198, 51, 100, 7)) && !webtv_public(IP4(203, 0, 113, 9)));
    CHECK(webtv_public(IP4(1, 1, 1, 1)) && webtv_public(IP4(223, 255, 255, 254)));
    T_REPORT("webtv");
}
