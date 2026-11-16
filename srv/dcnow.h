/* "Dreamcast Now": while a console is on the line, the site that shows who
 * is online is told so every 15 s (refs/dreampi-facts.md). The box is named
 * by the SHA-256 of its hardware address written as text, the game by the
 * SHA-256 of the last name the console asked a name server for. This file is
 * the hash and the rules of what is said when; sending it is the daemon's
 * business. Portable C99, integers only, no allocation. */
#ifndef V90D_SRV_DCNOW_H
#define V90D_SRV_DCNOW_H
#include <stddef.h>
#include <stdint.h>

#define DCNOW_HOST     "dcnow-2016.appspot.com"
#define DCNOW_AGENT    "Mozilla/4.0 (compatible; MSIE 5.5; Windows NT), Dreamcast Now"
#define DCNOW_EVERY_MS 15000u

/* SHA-256 (FIPS 180-4) of len octets as 64 lowercase hex digits and a 0 */
void sha256_hex(const void *data, size_t len, char out[65]);

typedef struct {
    char path[13 + 64 + 2];     /* "/api/update/<hash>/" */
    char last[254];             /* the last name worth reporting */
    int  once[2];               /* the two once-a-call names have been said */
} dcnow_t;

/* a call begins; mac is this box's hardware address */
void dcnow_start(dcnow_t *d, const uint8_t mac[6]);
const char *dcnow_path(const dcnow_t *d);
/* The body of the next report, given the last name the console asked for
 * ("" = none yet): "dns_query=<hash>" or nothing. Returns its length; with
 * too little room the body is empty. */
int dcnow_body(dcnow_t *d, const char *name, char *out, size_t cap);

#endif
