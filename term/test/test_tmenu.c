/* tmenu.h: the menu as a caller sees it. Everything it sends is played into
 * a test terminal (vt.h), which must then show exactly the menu's screen;
 * keys come in as a terminal sends them. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "tmenu.h"
#include "vt.h"

static tdir_t D;
static tmenu_t M;
static vt_t V;
static uint32_t now = 1000;
static uint8_t sent[1 << 20]; static size_t sent_n;     /* everything that went down the line */

static int tonight(void *ctx, int i, char l[20], char v[16], int *c)
{
    (void)ctx;
    if (i == 0) { strcpy(l, "Lines busy"); strcpy(v, "1 of 2"); *c = TS_LG; return 1; }
    if (i == 1) { strcpy(l, "This call"); strcpy(v, "V.90 50666"); *c = TS_YL; return 1; }
    return 0;
}
static int ready;
static int own(void *ctx, int id, tscr *s)
{
    (void)ctx;
    if (!ready) return TM_NOT_READY;
    tscr_put(s, 2, 2, id == TM_OWN ? "YOUR ACCOUNT" : "?", TS_WH, -1);
    return -1;
}
static int who(void *ctx, int i, tmenu_who *r)
{
    (void)ctx;
    if (i > 0) return 0;
    strcpy(r->node, "1"); strcpy(r->who, "Line 1"); strcpy(r->where, "this menu"); strcpy(r->speed, "V.90 50666");
    r->minutes = 2; r->you = 1;
    return 1;
}

/* the line takes everything; the terminal plays it */
static size_t drain(void)
{
    uint8_t b[512]; size_t n, all = 0;
    while ((n = tmenu_output(&M, b, sizeof b)) > 0) {
        vt_play(&V, b, n);
        if (sent_n + n < sizeof sent) { memcpy(sent + sent_n, b, n); sent_n += n; }
        all += n;
    }
    return all;
}
static void keys(const char *s) { tmenu_input(&M, (const uint8_t *)s, strlen(s), now); }
static void wait_ms(int ms) { int i; for (i = 0; i < ms; i++) { now++; tmenu_poll(&M, now); } }
static int shows(const char *text)          /* the text is somewhere on the caller's screen */
{
    char row[TSCR_COLS + 1]; int y, x;
    for (y = 0; y < TSCR_ROWS; y++) {
        for (x = 0; x < TSCR_COLS; x++) row[x] = (char)V.c[y * TSCR_COLS + x].ch;
        row[TSCR_COLS] = 0;
        if (strstr(row, text)) return 1;
    }
    return 0;
}
static int line_has(const char *s)
{
    size_t i, k = strlen(s);
    for (i = 0; i + k <= sent_n; i++) if (!memcmp(sent + i, s, k)) return 1;
    return 0;
}

static void setup(tmenu_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->title = "NETSTALGIA ONLINE"; c->dialed = "6387825442"; c->connect = "CONNECT 50666/V90/LAPM/V44";
    c->node = "Line 1"; c->speed = "V.90 50666/31200"; c->user = "Line 1";
    c->hello[0] = "Hello. This box answers on two lines.";
    c->items[0] = (tmenu_item){ '1', "Start PPP", "the Internet", TM_PPP };
    c->items[1] = (tmenu_item){ '2', "Who's online", "who is dialed in", TM_WHO };
    c->items[2] = (tmenu_item){ '3', "BBS directory", "3 boards to dial", TM_BBS };
    c->dir = &D; c->dir_source = "from the Telnet BBS Guide"; c->dir_note = "Dial 1-800-BBS-DIAL, then a board's name.";
    c->ppp_addr = "10.77.0.23"; c->ppp_server = "10.77.0.1"; c->ppp_mtu = 1500;
    c->tonight = tonight; c->who = who; c->lines = 2;
    c->items[3] = (tmenu_item){ '4', "Your account", "time, best", TM_OWN };
    c->draw = own;
}

int main(void)
{
    static const char *B[] = { "bbs alcatraz 2272522872 alcatrazbbs.ddns.net:9000 Alcatraz Prison BBS",
                               "bbs zruspa 2279787720 bbs.zruspas.org Zruspa's BBS",
                               "bbs floor13 2271384356 13th.hoyvision.com:6400 13th Floor BBS", NULL };
    static const char *I[] = { "board zruspa", "loc Madrid, Spain", "sw Synchronet", "nodes 4", "new NEW",
                               "upd 2020-04-18", "d A board in Spain.", NULL };
    tmenu_cfg c; int i, board = -1; size_t n;

    tdir_init(&D);
    for (i = 0; B[i]; i++) tdir_bbs_line(&D, B[i], strlen(B[i]));
    for (i = 0; I[i]; i++) tdir_info_line(&D, I[i], strlen(I[i]));
    tdir_sort(&D);
    setup(&c);

    /* the opening screen, whole, in colour */
    vt_init(&V);
    tmenu_init(&M, &c, now);
    n = drain();
    printf("    opening screen: %u octets\n", (unsigned)n);
    CHECK(n > 1000 && n < 12000);
    CHECK(!memcmp(sent, "\x1b[0m\x1b[2J", 8));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    CHECK(shows("CONNECT 50666") && shows("ANSI colour?") && shows("O  N  L  I  N  E"));
    CHECK_EQ_I(V.bad, 0);

    /* Y: the menu, whole */
    keys("y"); drain();
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    CHECK(shows("MAIN MENU") && shows("Tonight on the network") && shows("1 of 2"));
    /* an arrow (ANSI) moves the bar, as a few changes */
    sent_n = 0;
    keys("\x1b[B"); n = drain();
    CHECK(n > 0 && n < 400);
    CHECK_EQ_I(M.sel, 1);
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("\x1bOA"); drain();                         /* VT100 application mode */
    CHECK_EQ_I(M.sel, 0);
    keys("\x1b" "B"); drain();                       /* VT52 */
    CHECK_EQ_I(M.sel, 1);
    /* the bar wraps round to Goodbye */
    keys("\x1b[A\x1b[A"); drain();
    CHECK_EQ_I(M.sel, 4);

    /* 2: shown in "Your choice", then Who's online; any key back */
    keys("2"); drain();
    CHECK(shows("Your choice:") && M.screen == 1);
    wait_ms(230); drain();
    CHECK(shows("WHO'S ONLINE") && shows("(you)") && shows("of 2 lines busy"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("x"); drain();
    CHECK(shows("MAIN MENU"));

    /* 3: the directory, searched, a board's page, connect */
    keys("3"); wait_ms(240); drain();
    CHECK(shows("BBS DIRECTORY") && shows("3 boards") && shows("227-252-2872") && shows("page 1 of 1"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("z"); drain();                              /* a letter jumps */
    CHECK_EQ_I(M.dsel, 2);
    keys("/mad"); wait_ms(5); drain();
    CHECK(shows("Search: mad") && shows("1 found"));
    keys("\r"); drain();
    CHECK(!M.searching);
    keys("\r"); drain();
    CHECK(shows("ZRUSPA'S BBS") && shows("Madrid, Spain") && shows("Synchronet,  4 nodes") && shows("log in as NEW"));
    CHECK(shows("A board in Spain.") && shows("At the BBS: prompt") && shows("zruspa"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("c"); 
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_NONE);     /* not before the screen is out */
    drain();
    CHECK(shows("Putting you through to Zruspa's BBS"));
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_DIAL);
    CHECK_EQ_I(board, 1);
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_NONE);     /* once */
    vt_play(&V, (const uint8_t *)"\x1b[2JWelcome to the board", 22);   /* the board's own screens */
    tmenu_back(&M, now); drain();
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    CHECK(shows("BBS DIRECTORY") && shows("1 found"));
    /* Esc: a lone ESC is the key once nothing follows it */
    keys("\x1b"); drain();
    CHECK(shows("1 found"));
    wait_ms(310); drain();
    CHECK(shows("3 boards"));                        /* the search cleared */
    keys("\x1b"); wait_ms(310); drain();
    CHECK(shows("MAIN MENU"));
    CHECK(!line_has("ddns") && !line_has("zruspas.org") && !line_has(":9000"));

    /* 4: an owner's screen whose data comes later */
    ready = 0;
    keys("4"); wait_ms(240); drain();
    CHECK(shows("One moment..."));
    wait_ms(150); drain();
    CHECK(shows("One moment..."));
    ready = 1;
    wait_ms(150); drain();
    CHECK(shows("YOUR ACCOUNT") && !shows("One moment"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("x"); drain();
    CHECK(shows("MAIN MENU"));

    /* 1: the handoff, then PPP */
    keys("1"); wait_ms(230);
    drain();
    CHECK(shows("Starting PPP") && shows("10.77.0.23"));
    CHECK_EQ_I(tmenu_action(&M, NULL), TM_ACT_PPP);
    CHECK_EQ_I(tmenu_action(&M, NULL), TM_ACT_NONE);

    /* a slow line: keys pile up, the screens do not; in the end the caller
     * sees the last one exactly */
    vt_init(&V); sent_n = 0;
    tmenu_init(&M, &c, now); keys("Y");
    for (i = 0; i < 60; i++) { keys(i % 2 ? "\x1b[A" : "\x1b[B\x1b[B"); CHECK(M.out_n <= TM_OUT); }
    drain();
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    printf("    60 key presses on a line that took nothing meanwhile: %u octets in all\n", (unsigned)sent_n);
    /* the clock in the bars, once a minute */
    wait_ms(60000); n = drain();
    CHECK(n > 0 && n < 200);
    CHECK(shows("1:01") == 0 && shows("0:01"));
    /* G: goodbye, then hang up */
    keys("g"); drain();
    CHECK(shows("NO CARRIER"));
    CHECK_EQ_I(tmenu_action(&M, NULL), TM_ACT_HANGUP);

    /* without colour: no escape at all, ever */
    sent_n = 0;
    tmenu_init(&M, &c, now); drain();
    sent_n = 0;
    keys("n"); drain(); keys("3"); wait_ms(240); drain(); keys("+"); drain(); keys("\r"); drain();
    CHECK(sent_n > 0 && !memchr(sent, 0x1b, sent_n));
    CHECK(line_has("MAIN MENU") && line_has("BBS DIRECTORY") && line_has("+- move"));
    for (n = 0; n < sent_n; n++) if (sent[n] >= 0x80 || (sent[n] < 32 && sent[n] != '\r' && sent[n] != '\n')) break;
    CHECK_EQ_I(n, sent_n);                           /* plain ASCII only */
    /* 1-800-BBS-DIAL: the operator. The directory straight after the
     * opening screen, a BBS: line that narrows it as one types */
    c.operator = 1;
    vt_init(&V); sent_n = 0;
    tmenu_init(&M, &c, now); drain();
    CHECK(shows("ANSI colour?"));
    keys("y"); wait_ms(5); drain();
    CHECK(shows("BBS DIRECTORY") && shows("3 boards") && shows("BBS: ") && !shows("MAIN MENU"));
    CHECK(shows("type a board's name or number") && shows("Esc hang up"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("zru"); wait_ms(5); drain();
    CHECK(shows("BBS: zru") && shows("1 found") && shows("Zruspa's BBS") && !shows("Alcatraz"));
    CHECK_EQ_I(vt_diff(&V, &M.cur), 0);
    keys("\x7f\x7f\x7f"); wait_ms(5); drain();
    CHECK(shows("3 boards"));
    /* a name typed whole: through at once, not to its page */
    keys("Floor13\r"); drain();
    CHECK(shows("Putting you through to 13th Floor BBS"));
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_DIAL);
    CHECK_EQ_I(strcmp(tdir_get(&D, board, TD_LOGIN), "floor13"), 0);
    tmenu_back(&M, now); wait_ms(5); drain();
    CHECK(shows("BBS DIRECTORY") && shows("3 boards") && shows("BBS: "));
    /* a number, with the 1 and dashes */
    keys("1-227-978-7720\r"); drain();
    CHECK(shows("Putting you through to Zruspa's BBS"));
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_DIAL);
    CHECK_EQ_I(strcmp(tdir_get(&D, board, TD_LOGIN), "zruspa"), 0);
    tmenu_back(&M, now); wait_ms(5); drain();
    /* part of a name: Enter opens the board under the bar */
    keys("alca"); wait_ms(5); drain(); keys("\r"); drain();
    CHECK(shows("ALCATRAZ PRISON BBS") && shows("connect to it now"));
    keys("\x1b"); wait_ms(310); drain();
    CHECK(shows("BBS DIRECTORY") && shows("1 found"));
    /* Esc clears the line, Esc on an empty one says goodbye */
    keys("\x1b"); wait_ms(310); drain();
    CHECK(shows("3 boards"));
    keys("\x1b"); wait_ms(310); drain();
    CHECK(shows("NO CARRIER"));
    CHECK_EQ_I(tmenu_action(&M, NULL), TM_ACT_HANGUP);
    /* no match: nothing happens on Enter */
    tmenu_init(&M, &c, now); keys("y"); wait_ms(5); drain();
    keys("nosuchboard"); wait_ms(5); drain(); keys("\r"); drain();
    CHECK(shows("No board matches that") && M.screen == 5);
    CHECK_EQ_I(tmenu_action(&M, NULL), TM_ACT_NONE);
    /* without colour: the typing is echoed, the list follows on Enter */
    sent_n = 0;
    tmenu_init(&M, &c, now); drain();
    sent_n = 0;
    keys("n"); drain();
    CHECK(line_has("BBS DIRECTORY") && line_has("\r\nBBS: ") && line_has("Zruspa's BBS"));
    {   /* one screen, not an empty one and then the list */
        size_t i, screens = 0;
        for (i = 0; i + 13 <= sent_n; i++) if (!memcmp(sent + i, "BBS DIRECTORY", 13)) screens++;
        CHECK_EQ_I((long)screens, 1);
    }
    sent_n = 0;
    keys("zru"); drain();
    CHECK_EQ_I((long)sent_n, 3);                      /* only the echo */
    keys("\r"); drain();
    CHECK(line_has("1 found") && line_has("BBS: zru"));
    keys("spa\r"); drain();
    CHECK(line_has("Putting you through to Zruspa's BBS"));
    CHECK(!memchr(sent, 0x1b, sent_n));
    CHECK_EQ_I(tmenu_action(&M, &board), TM_ACT_DIAL);
    /* the lookup itself */
    CHECK_EQ_I(tdir_find(&D, "  ZRUSPA "), tdir_find(&D, "zruspa"));
    CHECK(tdir_find(&D, "zruspa") >= 0 && tdir_find(&D, "(227) 978.7720") == tdir_find(&D, "zruspa"));
    CHECK_EQ_I(tdir_find(&D, "227978772"), -1);
    CHECK_EQ_I(tdir_find(&D, "zrusp"), -1);
    CHECK_EQ_I(tdir_find(&D, ""), -1);
    T_REPORT("tmenu");
}
