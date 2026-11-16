/* v90arch - synthetic DNS: every A query gets an address from a private
 * pool, and the name <-> address pairs are remembered in a fixed table so
 * the HTTP and FTP side can turn a destination address back into the host
 * name the caller asked for. No upstream DNS. Addresses are host order. */
#ifndef V90ARCH_DNS_H
#define V90ARCH_DNS_H
#include <stddef.h>
#include <stdint.h>

#define DNS_TAB      512   /* names remembered; least recently used replaced */
#define DNS_NAME_MAX 128   /* longest name kept, with the NUL */
#define DNS_TTL      60

typedef struct {
    uint32_t addr;         /* 0: slot free */
    uint32_t hash;
    uint32_t used;         /* tick of the last use */
    char     name[DNS_NAME_MAX];
} dns_ent_t;

typedef struct {
    uint32_t  base, count; /* the pool: base .. base+count-1 */
    uint32_t  next;        /* offset of the next address to try */
    uint32_t  tick;
    uint32_t  ttl;
    dns_ent_t e[DNS_TAB];
} dns_t;

/* prefix 8..22 (the pool must be larger than the table). 0 ok, -1 bad. */
int dns_init(dns_t *d, uint32_t base, int prefix);
/* The address of a name, assigning one when new. 0 for an unusable name. */
uint32_t dns_assign(dns_t *d, const char *name);
/* The name an address was given to, or NULL. Counts as a use. */
const char *dns_name_of(dns_t *d, uint32_t addr);
int dns_in_pool(const dns_t *d, uint32_t addr);
/* One query packet in, one answer packet out. Returns the answer length,
 * 0 when the packet is to be ignored. */
size_t dns_answer(dns_t *d, const uint8_t *q, size_t qlen, uint8_t *out, size_t cap);
/* The host to ask the archive for: the Host header when there is one (a
 * pool address in it is turned back into its name), else the name of the
 * original destination address, else that address in dotted form. */
void dns_host_for(dns_t *d, const char *hdr_host, uint32_t orig, char *out, size_t cap);
void dns_dotted(uint32_t addr, char *out, size_t cap);
/* "a.b.c.d" -> host order address; the number of characters used, 0 if not one */
size_t dns_parse_quad(const char *s, uint32_t *addr);
#endif
