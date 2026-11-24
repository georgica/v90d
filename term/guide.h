/* v90d/term - board details for the terminal menu's BBS directory.
 *
 * The Telnet BBS Guide (telnetbbsguide.com, The BBS Corner: "a free list,
 * distribute it everywhere") publishes every month a text file,
 * full_<Mon>_<YY>.txt, an entry per board between lines of dashes:
 *
 *     ALCATRAZ PRISON BBS
 *     Last Updated: April 24, 2020
 *     Telnet: alcatrazbbs.ddns.net:9000
 *     ... Location: Allentown, PA, USA ... Software: BBS Express Pro
 *     Total Nodes: 5   Login: NEW
 *     (a description, any number of lines)
 *
 * gd_* reads it a line at a time. What a v90d box gets is not that file but
 * the details of the boards of its BBS list (srv/bbs.h), joined by
 * host:port, as Netstalgia's signed list "v90d-bbsinfo 1" (the tool
 * v90d-bbsinfo writes it; bi_* reads it):
 *
 *     v90d-bbsinfo 1
 *     board alcatraz           the board's login on the BBS list
 *     loc Allentown, PA, USA
 *     sw BBS Express Pro
 *     nodes 5
 *     new NEW                  what a new user logs in as
 *     web http://...
 *     upd 2020-04-24
 *     d <a line of the description>   (repeated; wrapped to 74 columns, 9 at most)
 *
 * UTF-8; '#' begins a comment; a key it does not know is passed over, so
 * fields can be added without a new version. Never a host or a port.
 *
 * Portable C99, no allocation, no globals. Ours; written for this project. */
#ifndef V90D_GUIDE_H
#define V90D_GUIDE_H
#include <stddef.h>
#include <stdint.h>

#define BI_HEAD       "v90d-bbsinfo 1"
#define BI_LOGIN_MAX  32
#define BI_FIELD_MAX  63              /* loc, sw, new, web */
#define BI_DESC_COLS  74
#define BI_DESC_LINES 9
#define BI_DESC_MAX   (BI_DESC_LINES * (BI_DESC_COLS + 1) * 2)   /* UTF-8 room */

typedef struct {
    char name[64];                    /* as the guide writes it (upper case) */
    char host[64];
    uint16_t port;                    /* 23 when the guide names none */
    char loc[BI_FIELD_MAX + 1], sw[BI_FIELD_MAX + 1], nodes[8], login[BI_FIELD_MAX + 1], web[BI_FIELD_MAX + 1];
    char upd[11];                     /* YYYY-MM-DD, "" when it does not read as a date */
    char desc[2048];                  /* the description, its lines joined by '\n' */
} gd_entry;

typedef struct {
    gd_entry e;
    int in, after_sw, have, dated;   /* dated: a "Last Updated:" line, which every board's entry has */
} gd_t;

void gd_init(gd_t *g);
/* One line of the guide (no line end; a CR at its end is allowed). Returns 1
 * when it finished an entry of a board: *out has it. Call gd_end() after the
 * last line for the one still open. */
int  gd_line(gd_t *g, const char *line, size_t len, gd_entry *out);
int  gd_end(gd_t *g, gd_entry *out);

/* "April 24, 2020" as "2020-04-24" into out[11]; 0 if it is not a date */
int  gd_date(const char *s, char out[11]);

/* Wrap UTF-8 text to cols columns: calls line(ctx, text, len) for each line,
 * at most max_lines of them (0 = no limit). Paragraphs ('\n') are kept;
 * a word longer than a line is cut. Returns the lines given. */
int  gd_wrap(const char *text, int cols, int max_lines, void (*line)(void *ctx, const char *s, size_t n), void *ctx);

/* the entry as lines of the list, under "board <login>", into out[0..cap);
 * returns the length (cap or more: it did not fit) */
size_t bi_write(const gd_entry *e, const char *login, char *out, size_t cap);

/* reading the list: one line at a time */
enum { BI_NONE = 0, BI_HEAD_LINE, BI_BOARD, BI_FIELD, BI_BAD };
/* For BI_BOARD *val is the login; for BI_FIELD *key is one of the keys
 * above (or one not known) and *val its text. val is not NUL terminated
 * where the line was not; lengths in *klen, *vlen. */
int  bi_line(const char *line, size_t len, const char **key, size_t *klen, const char **val, size_t *vlen);

#endif
