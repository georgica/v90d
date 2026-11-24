/* v90d/term - the terminal main menu: what a caller with a terminal program
 * (not Dial-Up Networking) gets on the Internet number. Netstalgia's
 * approved design (2026-11-24): an opening screen with the planet and the
 * wordmark and "ANSI colour? Y/n", a main menu with a highlight bar and
 * hotkeys, Start PPP (a framed handoff, then PPP on the same call), Who's
 * online, the BBS directory (tdir.h) with a page per board and "connect
 * to it", Goodbye. Plain ASCII for a terminal without colour.
 *
 * No input or output of its own: the owner gives it the caller's octets
 * (tmenu_input), calls tmenu_poll every few milliseconds, sends what
 * tmenu_output gives it, and does what tmenu_action asks (start PPP, put
 * the caller through to a board, hang up). Shared by v90d (srv/termsvc.c)
 * and Netstalgia's nsdiald, which adds its member screens as items of its
 * own (tmenu_item with an id >= TM_OWN).
 *
 * Portable C99, no allocation, no globals. Ours; written for this project. */
#ifndef V90D_TMENU_H
#define V90D_TMENU_H
#include <stddef.h>
#include <stdint.h>
#include "tscr.h"
#include "tdir.h"

#define TM_ITEMS   8              /* menu items before Goodbye */
#define TM_OUT     16384          /* output waiting for the line */
#define TM_PER     15             /* directory rows a page */
#define TM_OWN     100            /* first id of an owner's item */
#define TM_NOT_READY (-2)         /* cfg.draw: not yet */
#define TM_RETRY_MS 100

enum { TM_PPP = 1, TM_WHO, TM_BBS };                  /* the built-in items */
enum { TM_ACT_NONE = 0, TM_ACT_PPP, TM_ACT_DIAL, TM_ACT_HANGUP };

typedef struct { char key; const char *label, *hint; int id; } tmenu_item;

/* a row of Who's online */
typedef struct { char node[4], who[24], where[26], speed[14]; int minutes, you, anonymous; } tmenu_who;

typedef struct {
    void *ctx;
    const char *title;            /* the bars' name: "NETSTALGIA ONLINE" */
    const char *dialed;           /* "6387825442": the opening screen's ATDT line */
    const char *connect;          /* "CONNECT 50666/V90/LAPM/V44" */
    const char *node;             /* "Node 3" */
    const char *speed;            /* "V.90 50666/31200" */
    const char *user;             /* the caller's name, or a label ("Line 1") */
    const char *hello[3];         /* the opening screen's lines under the art (UTF-8) */
    tmenu_item  items[TM_ITEMS];  /* the menu, in order; key 0 ends it */
    tdir_t     *dir;              /* the BBS directory; NULL: no TM_BBS */
    const char *dir_source;       /* "from the Telnet BBS Guide, November 2026" */
    const char *dir_note;         /* the line under the directory: how to dial a board */
    int         lines;            /* Who's online: "busy of lines" */
    /* PPP's handoff: the caller's address, ours, the MTU */
    const char *ppp_addr, *ppp_server; int ppp_mtu;
    /* "Tonight on the network": up to five rows; 0 when there are no more */
    int  (*tonight)(void *ctx, int i, char label[20], char value[16], int *colour);
    /* Who's online: row i; 0 when there are no more */
    int  (*who)(void *ctx, int i, tmenu_who *row);
    /* An owner's screen (an item with id >= TM_OWN): draw it on s (the bars
     * are drawn already) and return where the cursor goes as y * 256 + x,
     * or -1. Any key then goes back to the menu. TM_NOT_READY: its data is
     * not there yet; the menu shows "One moment..." and asks again every
     * TM_RETRY_MS. */
    int  (*draw)(void *ctx, int id, tscr *s);
    /* the opening screen and the menu, after they are drawn: the owner's
     * own touches (screen 0 opening, 1 menu); may be NULL */
    void (*extra)(void *ctx, int screen, tscr *s);
} tmenu_cfg;

typedef struct {
    tmenu_cfg cfg;
    int       screen, sel, ansi, typed;
    int       item_id;            /* the owner's screen being shown */
    uint32_t  now, t0, go_at;     /* go_at: a hotkey's screen, shortly (0 none) */
    int       go_to;
    /* the directory */
    int       dsel, dpage, searching, dirty_search;
    char      q[TDIR_Q_MAX + 1];
    tdir_view view;               /* this caller's search */
    int       board;              /* index into the directory of the open board */
    /* keys */
    uint8_t   esc[8]; int esc_n; uint32_t esc_at; int last_cr;
    /* the line */
    tscr      cur, sent;
    int       dirty, full, minute;
    char      out[TM_OUT];
    char      scratch[TM_OUT];    /* a screen on its way into out */
    size_t    out_head, out_n;
    int       action, act_board, act_wait;
    int       not_ready; uint32_t retry_at;
} tmenu_t;

void tmenu_init(tmenu_t *m, const tmenu_cfg *cfg, uint32_t now_ms);
/* octets from the caller */
void tmenu_input(tmenu_t *m, const uint8_t *p, size_t n, uint32_t now_ms);
/* the menu's own time: the clock, keys that wait, the search */
void tmenu_poll(tmenu_t *m, uint32_t now_ms);
/* up to cap octets for the caller; returns how many */
size_t tmenu_output(tmenu_t *m, uint8_t *p, size_t cap);
/* What the owner is to do now, once tmenu_output has nothing left:
 * TM_ACT_PPP (the handoff is on the caller's screen: dce_start_ppp),
 * TM_ACT_DIAL (put the caller through to board *board of the directory;
 * tmenu_back() when that call is over), TM_ACT_HANGUP. Each is given once. */
int  tmenu_action(tmenu_t *m, int *board);
/* the caller is back from a board: the directory again, all of it */
void tmenu_back(tmenu_t *m, uint32_t now_ms);
/* the owner's data changed: the screen is made again (only what changed goes out) */
void tmenu_redraw(tmenu_t *m);

#endif
