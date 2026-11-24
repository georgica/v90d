/* v90arch - HTTP request head handling: find the end of the head, pick out
 * what the relay needs, and rewrite the head into the absolute-URI form a
 * forward proxy wants. Pure functions on buffers, no sockets. */
#ifndef V90ARCH_HTTP_H
#define V90ARCH_HTTP_H
#include <stddef.h>

#define HTTP_HOST_MAX 256

typedef struct {
    int    v09;                  /* "GET /path" with no version */
    int    chunked;              /* Transfer-Encoding other than identity: a body of unknown length */
    int    absolute;             /* target already has a scheme */
    long   body;                 /* Content-Length, 0 when none */
    char   method[16];
    char   host[HTTP_HOST_MAX];  /* Host header value, "" when none */
} http_req_t;

/* Length of a complete request head at the start of b (through the blank
 * line, or the single line of an HTTP/0.9 request), 0 while incomplete. */
size_t http_head_len(const char *b, size_t n);
/* 0 ok, -1 not a request we can relay */
int http_parse(const char *head, size_t hlen, http_req_t *r);
/* Write the head for the proxy. host is used for origin-form targets.
 * Returns the length written, -1 when it does not fit or cannot be done. */
long http_build(const char *head, size_t hlen, const http_req_t *r,
                const char *host, char *out, size_t cap);
/* Response head: its length (through the blank line; CRLF or bare LF) and
 * the status code; 0 while incomplete, -1 when it is not an HTTP response. */
long http_resp_head(const char *b, size_t n, int *status);
/* Value of a numeric header in a head, -1 when absent or not a number. */
long http_header_long(const char *head, size_t hlen, const char *name);
#endif
