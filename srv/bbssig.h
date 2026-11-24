/*
 * The bulletin board list a box fetches (fw/bbsfetch.sh) comes from
 * Netstalgia and is signed: its last line is
 *
 *     # sig ed25519 <128 lowercase hex digits>
 *
 * Ed25519 (RFC 8032) over every octet before that line, its own line end
 * included. To a list reader it is a comment. Inside the signed part, a
 *
 *     # serial <1 to 19 digits>
 *
 * line numbers the list; Netstalgia only ever raises it. A box takes a
 * fetched list only when one of the keys below has signed it and its
 * serial is above the one of the list it has, so an old signed list
 * cannot be handed to it again (DESIGN.md).
 */
#ifndef V90D_BBSSIG_H
#define V90D_BBSSIG_H
#include <stddef.h>
#include <stdint.h>

#define BBS_SIG_TAG      "# sig ed25519 "
#define BBS_SERIAL_TAG   "# serial "
#define BBS_SIG_KEYS     2
#define BBS_SIG_FILE_MAX (512u * 1024u)
enum { BBS_SIG_OK = 0, BBS_SIG_NONE, BBS_SIG_BAD };

/* Netstalgia's public keys (bbskeys.h); all zero = no key there */
extern const uint8_t bbs_sig_keys[BBS_SIG_KEYS][32];
/* BBS_SIG_OK: buf[0..n) ends in a signature line that one of keys signed;
 * BBS_SIG_NONE: no such line; BBS_SIG_BAD: a line, but no key signed it.
 * *serial (may be NULL): the list's serial, 0 when it has none (or more
 * than one, or a malformed one); only meaningful with BBS_SIG_OK. */
int bbs_sig_check(const char *buf, size_t n, const uint8_t (*keys)[32], int n_keys, uint64_t *serial);
/* the same for a file (at most BBS_SIG_FILE_MAX octets) */
int bbs_sig_file(const char *path, const uint8_t (*keys)[32], int n_keys, uint64_t *serial);
/* What to do with a fetched list: the result of its check and its serial,
 * against the serial of the list in force. */
enum { BBS_TAKE = 0, BBS_TAKE_UNSIGNED, BBS_TAKE_OLD, BBS_TAKE_SAME };
int bbs_take(int sig_result, uint64_t got, uint64_t have);
/* the serial line in buf[0..n): its value, 0 when there is none, more
 * than one, or a malformed one */
uint64_t bbs_serial(const char *buf, size_t n);
/* the same for a file (the list in the image, which is not checked) */
uint64_t bbs_serial_file(const char *path);

#endif
