/* The bulletin board numbers. A terminal program that dials one of them,
 * on a port of any service, is not given PPP: the call carries octets and
 * is joined to a bulletin board on the internet over telnet (the box as a
 * small access server).
 *
 *   1 800 BBS DIAL (1 800 227 3425)   the caller is asked which board
 *   227 and seven digits              one board each: 227 is BBS on the
 *                                     keys of a telephone, the seven are
 *                                     the board's login name on the same
 *                                     keys, zeros behind a short one
 *                                     (telehack: 227 835 3422)
 *
 * Both with or without a 1 in front. Here: the numbers, the list of boards,
 * and the line the caller types at the prompt. The network side is
 * srv/bbslink.c, the telnet words srv/telnet.h.
 *
 * The list is a text file. The box makes it from the Telnet BBS Guide's
 * archive, which it fetches from the guide once a day (fw/bbsfetch.sh);
 * a short one in the image (lists/bbs.txt) serves until then:
 *
 *     v90d-bbs 2                          the first line that is not a comment
 *     bbs telehack 2278353422 telehack.com:23 Telehack
 *         login name, telephone number, host[:port] (23), the board's name
 *
 * '#' begins a comment, empty lines are skipped. A login name is 1 to 32 of
 * a-z and 0-9; a number is 227 and seven digits (whoever writes the list
 * sees that no two boards share one: bbs_keypad() for the first, the next
 * free number for the others); a host is a name or address of letters,
 * digits, '.' and '-', and not the box itself. Where a board is, is the
 * list's business: a caller is shown names and numbers, and reaches only
 * boards of the list.
 *
 * Portable C99, no allocation, no globals. */
#ifndef V90D_BBS_H
#define V90D_BBS_H
#include <stddef.h>
#include <stdint.h>

#define BBS_HEAD       "v90d-bbs 2"
#define BBS_LOGIN_MAX  32
#define BBS_HOST_MAX   63
#define BBS_NAME_MAX   47
#define BBS_NUMBER      "8002273425"    /* 800 BBS DIAL */
#define BBS_NUMBER_LONG "18002273425"   /* with the 1 in front */
#define BBS_AREA        "227"           /* BBS: the boards' own numbers, 227 and BBS_NUMBER_LEN - 3 digits */
#define BBS_NUMBER_LEN  10
#define BBS_LINE_MAX   200         /* a line of the list, without its end */
#define BBS_TYPED_MAX  79          /* what the caller can type at the prompt */

typedef struct { char login[BBS_LOGIN_MAX + 1], number[BBS_NUMBER_LEN + 1], host[BBS_HOST_MAX + 1], name[BBS_NAME_MAX + 1]; uint16_t port; } bbs_entry;

enum { BBS_LINE_NONE = 0,      /* empty or a comment */
       BBS_LINE_HEAD,          /* "v90d-bbs 2" */
       BBS_LINE_BBS,           /* *e filled */
       BBS_LINE_BAD };
/* One line of the list (no line end; a trailing CR is allowed). */
int  bbs_line(const char *line, size_t len, bbs_entry *e);
/* What was dialled: BBS_NUM_PROMPT for 1 800 BBS DIAL, BBS_NUM_BOARD for a
 * board's number, which is put into board[] without the 1 in front. */
enum { BBS_NUM_NONE = 0, BBS_NUM_PROMPT, BBS_NUM_BOARD };
int  bbs_number(const char *dialed, char board[BBS_NUMBER_LEN + 1]);
/* A board's number from its login name. */
void bbs_keypad(const char *login, char out[BBS_NUMBER_LEN + 1]);
/* A number as people write it: "227 835 3422" (12 characters). */
void bbs_show(const char *number, char out[BBS_LOGIN_MAX + 1]);
/* What the caller typed, as a login name would be written: letters in
 * lower case and digits, everything else dropped ("20 For Beers" finds
 * 20forbeers). */
void bbs_key(const char *typed, char out[BBS_LOGIN_MAX + 1]);
/* Does the entry answer a search for the key (from bbs_key)? An empty key
 * finds every entry. */
int  bbs_match(const bbs_entry *e, const char *key);

/* The line the caller types. bbs_edit() takes one octet from the modem and
 * gives what is to be shown for it (echo[0..3), *echo_n); it returns 1 when
 * the line is finished (CR), and line[] then holds it. */
typedef struct { char line[BBS_TYPED_MAX + 1]; int n; } bbs_edit_t;
void bbs_edit_init(bbs_edit_t *t);
int  bbs_edit(bbs_edit_t *t, uint8_t oct, uint8_t echo[3], size_t *echo_n);

#endif
