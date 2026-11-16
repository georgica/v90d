/* Tests for the HTTP request head code: where a head ends (also when it
 * arrives in pieces), what is picked out of it, and the rewrite into the
 * absolute-URI form for the proxy. */
#include <string.h>
#include "test.h"
#include "http.h"

#define CHECK_EQ_S(got, want) do { if (strcmp((got), (want)) != 0) printf("    got:  %s\n    want: %s\n", (got), (want)); CHECK(strcmp((got), (want)) == 0); } while (0)

static char out[9000];

/* parse + build; returns the rewritten head or "<bad>" */
static const char *rw(const char *head, const char *host)
{
    http_req_t r;
    size_t hl = http_head_len(head, strlen(head));
    long n;
    if (hl == 0) return "<incomplete>";
    if (http_parse(head, hl, &r) != 0) return "<bad>";
    n = http_build(head, hl, &r, r.host[0] ? r.host : host, out, sizeof out - 1);
    if (n < 0) return "<nofit>";
    out[n] = '\0';
    return out;
}

static void test_head_len(void)
{
    const char *h = "GET /a HTTP/1.0\r\nHost: x\r\nAccept: */*\r\n\r\nBODY";
    size_t full = strlen(h) - 4, i;

    CHECK_EQ_I(http_head_len(h, strlen(h)), full);
    /* a head split across reads: incomplete at every earlier point */
    for (i = 0; i < full; i++) CHECK_EQ_I(http_head_len(h, i), 0);
    CHECK_EQ_I(http_head_len(h, full), full);
    /* bare LF line ends */
    CHECK_EQ_I(http_head_len("GET / HTTP/1.0\nHost: x\n\nrest", 27), 24);
    CHECK_EQ_I(http_head_len("GET / HTTP/1.0\n\n", 16), 16);
    CHECK_EQ_I(http_head_len("GET / HTTP/1.0\r\n\r\n", 18), 18);
    CHECK_EQ_I(http_head_len("GET / HTTP/1.0\r\n", 16), 0);
    /* HTTP/0.9: the one line is the whole request */
    CHECK_EQ_I(http_head_len("GET /path\r\n", 11), 11);
    CHECK_EQ_I(http_head_len("GET /path\n", 10), 10);
    CHECK_EQ_I(http_head_len("GET /path", 9), 0);
    CHECK_EQ_I(http_head_len("GET /path   \r\nxx", 16), 14);
    CHECK_EQ_I(http_head_len("", 0), 0);
    CHECK_EQ_I(http_head_len("\r\n", 2), 2);              /* an empty line: complete (and bad) */
}

static void test_parse(void)
{
    http_req_t r;
    const char *h;

    h = "POST /cgi-bin/x HTTP/1.0\r\nhost:  www.inode.com \r\nContent-length: 11\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), 0);
    CHECK_EQ_S(r.method, "POST");
    CHECK_EQ_S(r.host, "www.inode.com");
    CHECK_EQ_I(r.body, 11);
    CHECK_EQ_I(r.v09, 0);
    CHECK_EQ_I(r.absolute, 0);
    h = "GET /x\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), 0);
    CHECK_EQ_I(r.v09, 1);
    CHECK_EQ_S(r.host, "");
    CHECK_EQ_I(r.body, 0);
    h = "GET HTTP://www.inode.com/x HTTP/1.0\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), 0);
    CHECK_EQ_I(r.absolute, 1);
    /* not relayable */
    h = "CONNECT www.inode.com:443 HTTP/1.0\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "OPTIONS * HTTP/1.0\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "GET\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "AVERYLONGMETHODNAMEINDEED / HTTP/1.0\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "GET / HTTP/1.0\r\nContent-Length: -5\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    h = "GET / HTTP/1.0\r\nContent-Length: 99999999999999999999\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), -1);
    /* a Host value that could break the request line is not used */
    h = "GET / HTTP/1.0\r\nHost: a b\r\n\r\n";
    CHECK_EQ_I(http_parse(h, strlen(h), &r), 0);
    CHECK_EQ_S(r.host, "");
}

static void test_rewrite(void)
{
    /* with Host */
    CHECK_EQ_S(rw("GET /index.html HTTP/1.0\r\nHost: www.inode.com\r\nUser-Agent: Mozilla/3.0\r\n\r\n", "fallback"),
               "GET http://www.inode.com/index.html HTTP/1.0\r\nHost: www.inode.com\r\nUser-Agent: Mozilla/3.0\r\nConnection: close\r\n\r\n");
    /* without Host: the name of the destination address, and a Host line is added */
    CHECK_EQ_S(rw("GET / HTTP/1.0\r\nUser-Agent: Mozilla/1.0N\r\n\r\n", "www.inode.com"),
               "GET http://www.inode.com/ HTTP/1.0\r\nUser-Agent: Mozilla/1.0N\r\nHost: www.inode.com\r\nConnection: close\r\n\r\n");
    /* Host with a port */
    CHECK_EQ_S(rw("GET /a?b=c HTTP/1.0\r\nHost: www.inode.com:8080\r\n\r\n", "x"),
               "GET http://www.inode.com:8080/a?b=c HTTP/1.0\r\nHost: www.inode.com:8080\r\nConnection: close\r\n\r\n");
    /* absolute already: the request line is left alone */
    CHECK_EQ_S(rw("GET http://www.inode.com/x HTTP/1.0\r\nProxy-Connection: Keep-Alive\r\n\r\n", "fallback"),
               "GET http://www.inode.com/x HTTP/1.0\r\nConnection: close\r\n\r\n");
    CHECK_EQ_S(rw("GET ftp://ftp.inode.com/pub/ HTTP/1.0\r\nHost: ftp.inode.com\r\n\r\n", "fallback"),
               "GET ftp://ftp.inode.com/pub/ HTTP/1.0\r\nHost: ftp.inode.com\r\nConnection: close\r\n\r\n");
    /* HTTP/1.1 with keep-alive: downgraded, hop headers dropped (any case,
     * with their continuation lines) */
    CHECK_EQ_S(rw("GET /p HTTP/1.1\r\nHost: h.example\r\nConnection: Keep-Alive\r\nKEEP-ALIVE: 300,\r\n\tmax=5\r\nAccept: a,\r\n b\r\nproxy-connection: x\r\n\r\n", "x"),
               "GET http://h.example/p HTTP/1.0\r\nHost: h.example\r\nAccept: a,\r\n b\r\nConnection: close\r\n\r\n");
    /* HTTP/0.9 */
    CHECK_EQ_S(rw("GET /path\r\n", "www.inode.com"),
               "GET http://www.inode.com/path HTTP/1.0\r\nHost: www.inode.com\r\nConnection: close\r\n\r\n");
    CHECK_EQ_S(rw("GET /path\n", "www.inode.com"),
               "GET http://www.inode.com/path HTTP/1.0\r\nHost: www.inode.com\r\nConnection: close\r\n\r\n");
    /* POST with a body: the head only, the body is the relay's business */
    CHECK_EQ_S(rw("POST /cgi HTTP/1.0\r\nHost: www.inode.com\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\n\r\na=1&b=2", "x"),
               "POST http://www.inode.com/cgi HTTP/1.0\r\nHost: www.inode.com\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: 7\r\nConnection: close\r\n\r\n");
    /* bare LF heads keep their lines */
    CHECK_EQ_S(rw("GET / HTTP/1.0\nHost: a.example\nX: y\n\n", "x"),
               "GET http://a.example/ HTTP/1.0\r\nHost: a.example\nX: y\nConnection: close\r\n\r\n");
    /* extra spaces in the request line */
    CHECK_EQ_S(rw("GET   /x   HTTP/1.0\r\n\r\n", "h"),
               "GET http://h/x HTTP/1.0\r\nHost: h\r\nConnection: close\r\n\r\n");
    CHECK_EQ_S(rw("CONNECT a:443 HTTP/1.0\r\n\r\n", "h"), "<bad>");
}

static void test_long(void)
{
    static char head[8300], want[8600];
    http_req_t r;
    size_t n, hl;
    long m;

    /* long headers: an 8 kB head goes through whole */
    n = (size_t)snprintf(head, sizeof head, "GET /long HTTP/1.0\r\nHost: www.inode.com\r\nCookie: ");
    while (n < 8192 - 4) head[n++] = 'c';
    memcpy(head + n, "\r\n\r\n", 5); n += 4;
    CHECK_EQ_I(n, 8192);
    hl = http_head_len(head, n);
    CHECK_EQ_I(hl, 8192);
    CHECK_EQ_I(http_parse(head, hl, &r), 0);
    m = http_build(head, hl, &r, r.host, out, sizeof out);
    snprintf(want, sizeof want, "GET http://www.inode.com/long HTTP/1.0%.*sConnection: close\r\n\r\n",
             (int)(hl - 18 - 2), head + 18);
    CHECK_EQ_I(m, strlen(want));
    CHECK(m > 0 && memcmp(out, want, (size_t)m) == 0);
    /* and does not overrun a buffer that is too small */
    CHECK_EQ_I(http_build(head, hl, &r, r.host, out, 8192), -1);
    CHECK_EQ_I(http_build(head, hl, &r, r.host, out, 10), -1);
    CHECK_EQ_I(http_build(head, hl, &r, r.host, out, 0), -1);
    /* a Host value too long to keep counts as absent */
    n = (size_t)snprintf(head, sizeof head, "GET / HTTP/1.0\r\nHost: ");
    while (n < 600) head[n++] = 'h';
    memcpy(head + n, "\r\n\r\n", 5); n += 4;
    CHECK_EQ_I(http_parse(head, n, &r), 0);
    CHECK_EQ_S(r.host, "");
}

static void test_response(void)
{
    int st = 0;
    const char *a = "HTTP/1.0 200 OK\r\nContent-Length: 1131\r\nContent-type: text/plain\r\n\r\nbody";
    const char *b = "HTTP/1.0 404 Not Found\nDate: x\nConnection: close\n\n<html>";
    size_t i;

    CHECK_EQ_I(http_resp_head(a, strlen(a), &st), strlen(a) - 4);
    CHECK_EQ_I(st, 200);
    CHECK_EQ_I(http_header_long(a, strlen(a) - 4, "content-length"), 1131);
    CHECK_EQ_I(http_header_long(a, strlen(a) - 4, "Length"), -1);
    CHECK_EQ_I(http_header_long(a, strlen(a) - 4, "Content-Type"), -1);
    for (i = 0; i < strlen(a) - 4; i++) CHECK(http_resp_head(a, i, &st) <= 0);
    /* the proxy's own pages come with bare LF */
    CHECK_EQ_I(http_resp_head(b, strlen(b), &st), strlen(b) - 6);
    CHECK_EQ_I(st, 404);
    CHECK_EQ_I(http_resp_head("<html>no head", 13, &st), -1);
    CHECK_EQ_I(http_resp_head("HTTP/1.0 200 OK\r\n", 17, &st), 0);
    CHECK_EQ_I(http_resp_head("HTTP/1.0 banana\r\n\r\n", 19, &st), -1);
}

int main(void)
{
    test_head_len();
    test_parse();
    test_rewrite();
    test_long();
    test_response();
    T_REPORT("test_http");
}
