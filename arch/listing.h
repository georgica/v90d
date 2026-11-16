/* v90arch - turn the HTML directory listing the Protoweb proxy makes for
 * an ftp:// URL into the "ls -l" lines an FTP client expects. Works as a
 * stream: feed it whatever arrived, it emits whole lines. */
#ifndef V90ARCH_LISTING_H
#define V90ARCH_LISTING_H
#include <stddef.h>

#define LST_LINE_MAX 1024   /* longest HTML line looked at; longer ones are skipped */
#define LST_OUT_MAX  336    /* longest line produced */

typedef struct {
    int    names_only;      /* NLST */
    int    now_year, now_mon; /* for the "recent" date form; 0 = always year form */
    int    skip;            /* inside an overlong line */
    size_t n;
    char   line[LST_LINE_MAX];
} lst_t;

void lst_init(lst_t *l, int names_only, int now_year, int now_mon);
/* One HTML line (no newline) -> one output line with CRLF. 0: not an entry. */
size_t lst_entry(const lst_t *l, const char *line, size_t len, char *out, size_t cap);
/* Feed input; output goes to out (cap >= LST_OUT_MAX). Returns the input
 * consumed, which is less than n only when out filled up. */
size_t lst_feed(lst_t *l, const char *in, size_t n, char *out, size_t cap, size_t *outlen);
/* End of input: a last line without a newline. Returns the output length. */
size_t lst_finish(lst_t *l, char *out, size_t cap);
/* "1.2K" "3.4M" "825" "-" -> octets, approximately */
unsigned long long lst_size(const char *s, size_t n);
#endif
