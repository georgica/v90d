/*
 * The bulletin board list a box fetches (fw/bbsfetch.sh) comes from
 * Netstalgia and is signed: its last line is
 *
 *     # sig ed25519 <128 lowercase hex digits>
 *
 * Ed25519 (RFC 8032) over every octet before that line, its own line end
 * included. To a list reader it is a comment. A box takes a fetched list
 * only when one of the keys below has signed it (DESIGN.md).
 */
#ifndef V90D_BBSSIG_H
#define V90D_BBSSIG_H
#include <stddef.h>
#include <stdint.h>

#define BBS_SIG_TAG      "# sig ed25519 "
#define BBS_SIG_KEYS     2
#define BBS_SIG_FILE_MAX (512u * 1024u)
enum { BBS_SIG_OK = 0, BBS_SIG_NONE, BBS_SIG_BAD };

/* Netstalgia's public keys (bbskeys.h); all zero = no key there */
extern const uint8_t bbs_sig_keys[BBS_SIG_KEYS][32];
/* BBS_SIG_OK: buf[0..n) ends in a signature line that one of keys signed;
 * BBS_SIG_NONE: no such line; BBS_SIG_BAD: a line, but no key signed it */
int bbs_sig_check(const char *buf, size_t n, const uint8_t (*keys)[32], int n_keys);
/* the same for a file (at most BBS_SIG_FILE_MAX octets) */
int bbs_sig_file(const char *path, const uint8_t (*keys)[32], int n_keys);

#endif
