/* v90d/term - the BBS directory of the terminal menu: every board of the
 * BBS list (srv/bbs.h: login, 227 number, name) with the details of the
 * board list "v90d-bbsinfo 1" (guide.h) where there are any, a search over
 * names, places, software and descriptions, and what the board pages show.
 * Never a host or a port: those stay in the BBS list.
 *
 * Filled a line at a time (tdir_bbs_line for the BBS list, then
 * tdir_info_line for the details), searched a slice at a time (tdir_step),
 * so that an owner with a real-time loop can spread the work. The strings
 * live in one pool inside tdir_t, about 300 KB in all. Once filled it is
 * only read: each caller's search is a tdir_view of its own.
 *
 * Portable C99, no allocation, no globals. Ours; written for this project. */
#ifndef V90D_TDIR_H
#define V90D_TDIR_H
#include <stddef.h>
#include <stdint.h>

#define TDIR_MAX   2048                 /* boards */
#define TDIR_POOL  (320u * 1024u)       /* their text */
#define TDIR_Q_MAX 38                   /* a search */
#define TDIR_HASH  4096

enum { TD_NAME, TD_NUMBER, TD_LOGIN, TD_LOC, TD_SW, TD_NODES, TD_NEW, TD_WEB, TD_UPD, TD_DESC, TD_FIELDS };

typedef struct { uint32_t f[TD_FIELDS]; } tdir_board;   /* pool offsets; 0 = none */

typedef struct {
    tdir_board b[TDIR_MAX];
    int        n;                       /* boards */
    char       pool[TDIR_POOL];
    uint32_t   used;
    uint16_t   hash[TDIR_HASH];         /* login -> board + 1 */
    int        cur;                     /* the board tdir_info_line is filling, -1 none */
    int        with_info;               /* boards that got details */
    uint16_t   order[TDIR_MAX];         /* the boards by name (tdir_sort) */
    int        sorted;
} tdir_t;

/* one caller's search */
typedef struct {
    char       q[TDIR_Q_MAX + 1];
    uint16_t   res[TDIR_MAX];
    int        nres, scanned;           /* matches so far; boards looked at */
} tdir_view;

void tdir_init(tdir_t *d);
/* a line of the BBS list; boards past TDIR_MAX or the pool are left out */
void tdir_bbs_line(tdir_t *d, const char *line, size_t len);
/* a line of the board details; a board the list does not have is passed over */
void tdir_info_line(tdir_t *d, const char *line, size_t len);
/* once filled: the boards in the order of their names (letters without
 * case), which searches and the letter jumps follow. Shell sort: some
 * 15 000 comparisons for 1 000 boards. */
void tdir_sort(tdir_t *d);

/* a new search (q "" = every board, by name once sorted); the results grow
 * with each tdir_step(), which looks at up to `budget` boards and returns 1
 * once every board has been looked at */
void tdir_search(tdir_view *v, const char *q);
int  tdir_step(const tdir_t *d, tdir_view *v, int budget);
int  tdir_done(const tdir_t *d, const tdir_view *v);

/* field f of board i (an index into d->b), "" when it has none */
const char *tdir_get(const tdir_t *d, int i, int f);
/* the first result (from 0) whose name begins with the letter (A-Z, or '#'
 * for anything else), -1 when none does */
int  tdir_letter(const tdir_t *d, const tdir_view *v, char letter);
/* the letter a name is filed under: 'A'..'Z' or '#' */
char tdir_file_as(const char *name);

#endif
