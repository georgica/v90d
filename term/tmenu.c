/* See tmenu.h. The screens follow Netstalgia's approved mock-up
 * (docs/superpowers/specs/assets/2026-11-24-terminal-menu-mockup.src.html in
 * georgica/netstalgia-sip): the same places, characters and colours. */
#include <stdio.h>
#include <string.h>
#include "tmenu.h"
#include "tart.h"
#include "guide.h"

enum { S_CONNECT, S_MENU, S_PPP, S_WHO, S_OWN, S_BBS, S_BOARD, S_DIAL, S_BYE };
enum { K_UP = 256, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC, K_BS, K_PGUP, K_PGDN };

#define HOTKEY_MS 220             /* a hotkey shows in "Your choice" this long first */
#define ESC_MS    300             /* a lone ESC is the Esc key after this */
#define SEARCH_STEP 128           /* boards looked at per poll */
#define DIR_BUDGET (TM_OUT / 4)   /* output not yet gone before the next screen is made */

/* ---- small text helpers ---------------------------------------------- */

/* the first n characters of UTF-8 s into out, "…" (shown as '.') at the end if cut */
static const char *cut(const char *s, int n, char *out, size_t cap)
{
    const char *p = s;
    size_t k = 0;
    int c = 0, len = tscr_len(s);
    if (len <= n) { snprintf(out, cap, "%s", s); return out; }
    while (*p && c < n - 1) {
        size_t l = 1;
        unsigned char ch = (unsigned char)*p;
        if (ch >= 0xC0) while ((p[l] & 0xC0) == 0x80 && l < 4) l++;
        if (k + l + 4 >= cap) break;
        memcpy(out + k, p, l); k += l; p += l; c++;
    }
    memcpy(out + k, "\xe2\x80\xa6", 3); k += 3;
    out[k] = '\0';
    return out;
}

static const char *thousands(int v, char *out)
{
    char b[16];
    int n = snprintf(b, sizeof b, "%d", v), i, k = 0;
    for (i = 0; i < n; i++) {
        out[k++] = b[i];
        if ((n - i - 1) % 3 == 0 && i < n - 1) out[k++] = ',';
    }
    out[k] = '\0';
    return out;
}

/* "2270933223" as "227-093-3223" */
static const char *number(const char *n, char out[16])
{
    if (strlen(n) != 10) { snprintf(out, 16, "%s", n); return out; }
    snprintf(out, 16, "%.3s-%.3s-%s", n, n + 3, n + 6);
    return out;
}

static void clock_of(const tmenu_t *m, char out[12])
{
    uint32_t min = (m->now - m->t0) / 60000u;
    snprintf(out, 12, "%u:%02u", (unsigned)(min / 60), (unsigned)(min % 60));
}

static int nitems(const tmenu_t *m)
{
    int n = 0;
    while (n < TM_ITEMS && m->cfg.items[n].key) n++;
    return n;
}

/* ---- the frame -------------------------------------------------------- */

static void top(tmenu_t *m, tscr *s)
{
    char l[64], r[96], c[12];
    clock_of(m, c);
    snprintf(l, sizeof l, " %s ░▒▓", m->cfg.title ? m->cfg.title : "NETSTALGIA ONLINE");
    snprintf(r, sizeof r, "%s │ %s │ %s ", m->cfg.node ? m->cfg.node : "Node 1", m->cfg.speed ? m->cfg.speed : "", c);
    tscr_bar(s, 0, l, r, TS_WH, TS_B);
}

static void foot(tmenu_t *m, tscr *s, const char *left, const char *right)
{
    tscr_bar(s, TSCR_ROWS - 1, "", "", TS_K, TS_C);
    tscr_put(s, 1, TSCR_ROWS - 1, left, TS_K, TS_C);
    tscr_put(s, TSCR_COLS - 1 - tscr_len(right), TSCR_ROWS - 1, right, TS_K, TS_C);
    (void)m;
}

static void foot_online(tmenu_t *m, tscr *s, const char *left)
{
    char r[64], c[12];
    clock_of(m, c);
    snprintf(r, sizeof r, "%s │ online %s", m->cfg.user ? m->cfg.user : "", c);
    foot(m, s, left, r);
}

/* ---- the screens ------------------------------------------------------ */

static int draw_connect(tmenu_t *m, tscr *s)
{
    char b[64];
    int i;
    snprintf(b, sizeof b, "ATDT%s", m->cfg.dialed ? m->cfg.dialed : "");
    tscr_put(s, 0, 0, b, TS_W, -1);
    tscr_put(s, 0, 1, m->cfg.connect ? m->cfg.connect : "CONNECT", TS_WH, -1);
    tart_planet(s, 15, 15, 8);
    tart_wordmark(s, 31, 9);
    tscr_flush(s);
    tscr_put(s, 32, 13, "O  N  L  I  N  E", TS_YL, -1);
    tscr_put(s, 52, 13, "since 1999", TS_DK, -1);
    tscr_put(s, 32, 14, "unlimited dial-up Internet", TS_W, -1);
    tscr_fill(s, 0, 16, TSCR_COLS, 1, "▄", TS_DK, -1);
    for (i = 0; i < 3; i++) if (m->cfg.hello[i]) tscr_put(s, 3, 18 + i, m->cfg.hello[i], TS_W, -1);
    tscr_put(s, 3, 22, "ANSI colour? ", TS_WH, -1);
    tscr_put(s, 16, 22, " Y ", TS_K, TS_C);
    tscr_put(s, 20, 22, "/ n", TS_W, -1);
    snprintf(b, sizeof b, "%s │ press Y or N", m->cfg.user ? m->cfg.user : "");
    foot(m, s, "Welcome to Netstalgia Online", b);
    if (m->cfg.extra) m->cfg.extra(m->cfg.ctx, 0, s);
    return 22 * 256 + 24;
}

static int draw_menu(tmenu_t *m, tscr *s)
{
    int n = nitems(m), i, x, y, gy, rows = 0;
    char k[8], keys[48];
    top(m, s);
    tscr_put(s, 2, 2, "MAIN MENU", TS_WH, -1);
    tscr_fill(s, 13, 2, 65, 1, "─", TS_DK, -1);
    for (i = 0; i <= n; i++) {
        const char *label = i < n ? m->cfg.items[i].label : "Goodbye", *hint = i < n ? m->cfg.items[i].hint : "hang up";
        char key = i < n ? m->cfg.items[i].key : 'G';
        y = i < n ? 4 + i : 4 + n + 1;
        if (i == m->sel) {
            char b[64];
            tscr_fill(s, 1, y, 44, 1, " ", TS_K, TS_C);
            snprintf(b, sizeof b, "►%c) %s", key, label);
            tscr_put(s, 2, y, b, TS_K, TS_C);
            tscr_put(s, 22, y, hint ? hint : "", TS_K, TS_C);
        } else {
            k[0] = key; k[1] = '\0';
            tscr_put(s, 3, y, k, TS_YL, -1);
            tscr_put(s, 4, y, ") ", TS_W, -1);
            tscr_put(s, 6, y, label, TS_WH, -1);
            tscr_put(s, 22, y, hint ? hint : "", TS_DK, -1);
        }
    }
    gy = 4 + n + 1;
    (void)gy;
    if (m->cfg.tonight) {
        char label[20], value[16];
        int col;
        tscr_box(s, 48, 4, 31, 7, TS_LB, 0, "Tonight on the network", TS_LC);
        for (i = 0; i < 5; i++) {
            label[0] = value[0] = '\0'; col = TS_WH;
            if (!m->cfg.tonight(m->cfg.ctx, i, label, value, &col)) break;
            tscr_put(s, 50, 5 + i, label, TS_W, -1);
            tscr_put(s, 67, 5 + i, value, col, -1);
            rows++;
        }
    }
    (void)rows;
    {
        static const uint8_t grad[7] = { TS_B, TS_C, TS_LC, TS_WH, TS_LC, TS_C, TS_B };
        for (x = 0; x < TSCR_COLS; x++) tscr_dot(s, x, 34, grad[(x / 4) % 7]);
        tscr_flush(s);
    }
    tscr_put(s, 2, 19, "Your choice: ", TS_WH, -1);
    k[0] = ' ';
    k[1] = m->typed ? (char)m->typed : (m->sel < n ? m->cfg.items[m->sel].key : 'G');
    k[2] = ' '; k[3] = '\0';
    tscr_put(s, 15, 19, k, TS_K, TS_C);
    if (n > 0) snprintf(keys, sizeof keys, "%s   Enter choose   %c-%c or G", m->ansi ? "↑↓ move" : "+- move",
                        m->cfg.items[0].key, m->cfg.items[n - 1].key);
    else snprintf(keys, sizeof keys, "G hangs up");
    foot_online(m, s, keys);
    if (m->cfg.extra) m->cfg.extra(m->cfg.ctx, 1, s);
    return 19 * 256 + 18;
}

static int draw_ppp(tmenu_t *m, tscr *s)
{
    char b[24];
    top(m, s);
    tscr_box(s, 2, 2, 76, 7, TS_LB, 1, NULL, 0);
    tscr_put(s, 5, 3, "Starting PPP", TS_WH, -1);
    tscr_put(s, 5, 4, "Your address", TS_W, -1); tscr_put(s, 18, 4, m->cfg.ppp_addr ? m->cfg.ppp_addr : "by PPP", TS_WH, -1);
    tscr_put(s, 32, 4, "Server", TS_W, -1);       tscr_put(s, 39, 4, m->cfg.ppp_server ? m->cfg.ppp_server : "", TS_WH, -1);
    snprintf(b, sizeof b, "%d", m->cfg.ppp_mtu > 0 ? m->cfg.ppp_mtu : 1500);
    tscr_put(s, 52, 4, "MTU", TS_W, -1);          tscr_put(s, 56, 4, b, TS_WH, -1);
    tscr_put(s, 5, 5, "Start your PPP software now: Trumpet Winsock, pppd, MacPPP.", TS_W, -1);
    tscr_put(s, 5, 6, "To leave instead, type ", TS_W, -1);
    tscr_put(s, 28, 6, "+++", TS_WH, -1);
    tscr_put(s, 32, 6, "and hang up.", TS_W, -1);
    tscr_put(s, 2, 11, "Negotiating ", TS_W, -1);
    tscr_fill(s, 15, 11, 40, 1, "░", TS_DK, -1);
    tscr_put(s, 57, 11, "LCP...", TS_W, -1);
    tscr_put(s, 2, 13, "(what follows is PPP talking: your PPP software takes it from here)", TS_DK, -1);
    foot(m, s, "PPP", m->cfg.ppp_addr ? m->cfg.ppp_addr : "");
    return 14 * 256 + 2;
}

static int draw_who(tmenu_t *m, tscr *s)
{
    tmenu_who r;
    char b[32];
    int i, busy = 0;
    top(m, s);
    tscr_put(s, 2, 2, "WHO'S ONLINE", TS_WH, -1);
    tscr_fill(s, 16, 2, 40, 1, "─", TS_DK, -1);
    tscr_bar(s, 4, "  Node  Caller                Where                    Speed         Online", "", TS_K, TS_M);
    for (i = 0; i < 14 && m->cfg.who; i++) {
        int y = 5 + i, bg = i % 2 ? TS_B : TS_K;
        memset(&r, 0, sizeof r);
        if (!m->cfg.who(m->cfg.ctx, i, &r)) break;
        busy++;
        tscr_fill(s, 0, y, TSCR_COLS, 1, " ", TS_W, bg);
        tscr_put(s, 4, y, r.node, TS_W, bg);
        tscr_put(s, 8, y, r.who, r.anonymous ? TS_DK : TS_WH, bg);
        if (r.you) tscr_put(s, 8 + tscr_len(r.who) + 1, y, "(you)", TS_C, bg);
        tscr_put(s, 30, y, r.where, TS_W, bg);
        tscr_put(s, 55, y, r.speed, TS_LG, bg);
        snprintf(b, sizeof b, "%3d min", r.minutes);
        tscr_put(s, 69, y, b, TS_W, bg);
    }
    snprintf(b, sizeof b, "%d", busy);
    tscr_put(s, 58, 2, b, TS_LG, -1);
    snprintf(b, sizeof b, "of %d %s busy", m->cfg.lines > 0 ? m->cfg.lines : busy, (m->cfg.lines == 1) ? "line" : "lines");
    tscr_put(s, 58 + (busy > 9 ? 3 : 2), 2, b, TS_W, -1);
    tscr_put(s, 2, 22, "Press any key for the main menu.", TS_WH, -1);
    foot_online(m, s, "Who's online");
    return 22 * 256 + 35;
}

static int draw_own(tmenu_t *m, tscr *s)
{
    int cur;
    top(m, s);
    cur = m->cfg.draw ? m->cfg.draw(m->cfg.ctx, m->item_id, s) : -1;
    m->not_ready = cur == TM_NOT_READY;
    if (m->not_ready) {                   /* its data is on the way: asked again shortly */
        tscr_clear(s);
        top(m, s);
        tscr_put(s, 2, 3, "One moment...", TS_W, -1);
        foot_online(m, s, "");
        m->retry_at = m->now + TM_RETRY_MS;
        return 3 * 256 + 16;
    }
    return cur;
}

static int pages(const tmenu_t *m) { int n = m->view.nres; return n ? (n + TM_PER - 1) / TM_PER : 1; }

static int draw_bbs(tmenu_t *m, tscr *s)
{
    const tdir_t *d = m->cfg.dir;
    char b[128], c[64], num[16], t[16];
    int i, first, k, np = pages(m), n = m->view.nres;
    const char *letters = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char curl = 0;
    top(m, s);
    if (m->dpage > np - 1) m->dpage = np - 1;
    if (m->dsel > n - 1) m->dsel = n > 0 ? n - 1 : 0;
    tscr_put(s, 2, 2, "BBS DIRECTORY", TS_WH, -1);
    snprintf(b, sizeof b, "%s %s", thousands(n, t), m->q[0] ? "found" : "boards");
    tscr_put(s, 17, 2, b, TS_YL, -1);
    if (m->cfg.dir_source) {
        snprintf(c, sizeof c, "· %s", m->cfg.dir_source);
        tscr_put(s, 17 + tscr_len(b) + 1, 2, c, TS_DK, -1);
    }
    if (m->cfg.operator) {
        tscr_put(s, 2, 3, "BBS: ", TS_WH, -1);
        tscr_fill(s, 7, 3, 40, 1, " ", TS_K, TS_C);
        tscr_put(s, 7, 3, m->q, TS_K, TS_C);
        if (!m->q[0]) tscr_put(s, 49, 3, "type a board's name or number", TS_DK, -1);
    } else {
    tscr_put(s, 2, 3, "Search: ", TS_W, -1);
    if (m->searching) { tscr_fill(s, 10, 3, 40, 1, " ", TS_K, TS_C); tscr_put(s, 10, 3, m->q, TS_K, TS_C); }
    else tscr_put(s, 10, 3, m->q[0] ? m->q : "(press / to search names, places, software and descriptions)", m->q[0] ? TS_WH : TS_DK, -1);
    }
    tscr_bar(s, 4, "  Board                         Software       Location            Dial", "", TS_K, TS_M);
    first = m->dpage * TM_PER;
    for (i = 0; i < TM_PER && first + i < n; i++) {
        int bi = m->view.res[first + i], y = 5 + i, on = first + i == m->dsel;
        int bg = on ? TS_C : TS_K, nm = on ? TS_K : TS_WH, tx = on ? TS_K : TS_W;
        const char *nb = tdir_get(d, bi, TD_NUMBER), *sw = tdir_get(d, bi, TD_SW);
        tscr_fill(s, 0, y, TSCR_COLS, 1, " ", tx, bg);
        if (on) tscr_put(s, 0, y, "►", TS_K, bg);
        tscr_put(s, 2, y, cut(tdir_get(d, bi, TD_NAME), 29, c, sizeof c), nm, bg);
        tscr_put(s, 32, y, cut(sw[0] ? sw : "?", 14, c, sizeof c), tx, bg);
        tscr_put(s, 47, y, cut(tdir_get(d, bi, TD_LOC), 19, c, sizeof c), tx, bg);
        tscr_put(s, 67, y, nb[0] ? number(nb, num) : "   not yet", nb[0] ? (on ? TS_K : TS_LG) : (on ? TS_K : TS_DK), bg);
    }
    if (!n && tdir_done(d, &m->view))
        tscr_put(s, 2, 6, !m->view.q[0] ? "The board list has not arrived yet."
                          : m->cfg.operator ? "No board matches that. Esc clears the line."
                          : "No board matches that. Press / to search again, Esc to clear.", TS_W, -1);
    if (n) curl = tdir_file_as(tdir_get(d, m->view.res[m->dsel], TD_NAME));
    for (k = 0; letters[k]; k++) { char l[2] = { letters[k], 0 }; tscr_put(s, 2 + k * 2, 21, l, letters[k] == curl ? TS_YL : TS_DK, -1); }
    snprintf(b, sizeof b, "page %d of %d", m->dpage + 1, np);
    tscr_put(s, 58, 21, b, TS_W, -1);
    tscr_put(s, 2, 22, m->cfg.dir_note ? m->cfg.dir_note : "", TS_DK, -1);
    if (m->cfg.operator)
        snprintf(b, sizeof b, "%s  %s  Enter call or open  Esc %s",
                 m->ansi ? "↑↓ move" : "^E ^X move", m->ansi ? "←→ page" : "<> page", m->q[0] ? "clear" : "hang up");
    else snprintf(b, sizeof b, "%s  %s  A-Z jump  / search  Enter open  Esc menu",
             m->ansi ? "↑↓ move" : "+- move", m->ansi ? "←→ page" : "<> page");
    {
        char r[48], cl[12];
        clock_of(m, cl);
        snprintf(r, sizeof r, "%s │ %s", m->cfg.user ? m->cfg.user : "", cl);
        foot(m, s, b, r);
    }
    if (m->cfg.operator) return 3 * 256 + 7 + tscr_len(m->q);
    return m->searching ? 3 * 256 + 10 + tscr_len(m->q) : -1;
}

typedef struct { tscr *s; int y, n; } wrap_ctx;
static void desc_line(void *ctx, const char *p, size_t n)
{
    wrap_ctx *w = ctx;
    char b[BI_DESC_COLS * 4 + 1];
    if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, p, n); b[n] = '\0';
    tscr_put(w->s, 3, w->y + w->n, b, TS_W, -1);
    w->n++;
}

static int draw_board(tmenu_t *m, tscr *s)
{
    const tdir_t *d = m->cfg.dir;
    int i = m->board, r;
    char name[64], b[160], c[96], num[16];
    const char *nb = tdir_get(d, i, TD_NUMBER), *nodes = tdir_get(d, i, TD_NODES), *nl = tdir_get(d, i, TD_NEW);
    const char *upd = tdir_get(d, i, TD_UPD), *desc = tdir_get(d, i, TD_DESC);
    struct { const char *label; const char *val; int col; } rows[7];
    wrap_ctx w = { s, 13, 0 };
    char sw[96], newu[96], dial[64], up[32];
    size_t k;
    top(m, s);
    snprintf(name, sizeof name, "%s", tdir_get(d, i, TD_NAME));
    for (k = 0; name[k]; k++) if (name[k] >= 'a' && name[k] <= 'z') name[k] = (char)(name[k] - 32);
    tscr_box(s, 1, 2, 78, 10, TS_LB, 1, cut(name, 60, c, sizeof c), TS_WH);
    if (nb[0]) snprintf(dial, sizeof dial, "%s  (ATDT%s)", number(nb, num), nb);
    else snprintf(dial, sizeof dial, "not on 1-800-BBS-DIAL yet");
    snprintf(sw, sizeof sw, "%s%s%s%s", tdir_get(d, i, TD_SW)[0] ? tdir_get(d, i, TD_SW) : "-",
             nodes[0] ? ",  " : "", nodes, nodes[0] ? (strcmp(nodes, "1") ? " nodes" : " node") : "");
    if (nl[0]) snprintf(newu, sizeof newu, "log in as %s", nl); else snprintf(newu, sizeof newu, "-");
    snprintf(up, sizeof up, "updated %s", upd[0] ? upd : "-");
    rows[0].label = "Dial";               rows[0].val = dial; rows[0].col = nb[0] ? TS_LG : TS_DK;
    rows[1].label = "At the BBS: prompt"; rows[1].val = tdir_get(d, i, TD_LOGIN); rows[1].col = TS_WH;
    rows[2].label = "Location";           rows[2].val = tdir_get(d, i, TD_LOC)[0] ? tdir_get(d, i, TD_LOC) : "-"; rows[2].col = TS_WH;
    rows[3].label = "Software";           rows[3].val = sw; rows[3].col = TS_WH;
    rows[4].label = "New users";          rows[4].val = newu; rows[4].col = TS_WH;
    rows[5].label = "Website";            rows[5].val = tdir_get(d, i, TD_WEB)[0] ? tdir_get(d, i, TD_WEB) : "-"; rows[5].col = TS_LC;
    rows[6].label = "In the guide";       rows[6].val = up; rows[6].col = TS_DK;
    for (r = 0; r < 7; r++) {
        tscr_put(s, 4, 3 + r, rows[r].label, TS_W, -1);
        tscr_put(s, 24, 3 + r, cut(rows[r].val, 52, b, sizeof b), rows[r].col, -1);
    }
    /* the description, wrapped again here (the list may wrap it otherwise) */
    (void)gd_wrap(desc[0] ? desc : "No description in the guide.", BI_DESC_COLS, BI_DESC_LINES, desc_line, &w);
    tscr_put(s, 2, 23, "C", TS_YL, -1);   tscr_put(s, 3, 23, ") connect to it now", TS_WH, -1);
    tscr_put(s, 28, 23, "Esc", TS_YL, -1); tscr_put(s, 31, 23, ") back to the list", TS_WH, -1);
    {
        char l[96], r2[48], cl[12];
        clock_of(m, cl);
        snprintf(l, sizeof l, "BBS directory · %s", cut(tdir_get(d, i, TD_NAME), 40, c, sizeof c));
        snprintf(r2, sizeof r2, "%s │ %s", m->cfg.user ? m->cfg.user : "", cl);
        foot(m, s, l, r2);
    }
    return 23 * 256 + 50;
}

static int draw_dial(tmenu_t *m, tscr *s)
{
    const tdir_t *d = m->cfg.dir;
    char b[160];
    const char *loc = tdir_get(d, m->board, TD_LOC);
    snprintf(b, sizeof b, "BBS: %s", tdir_get(d, m->board, TD_LOGIN));
    tscr_put(s, 0, 0, b, TS_WH, -1);
    snprintf(b, sizeof b, "Putting you through to %s...", tdir_get(d, m->board, TD_NAME));
    tscr_put(s, 0, 2, b, TS_W, -1);
    snprintf(b, sizeof b, "Ringing %s.", loc[0] ? loc : "the board");
    tscr_put(s, 0, 3, b, TS_DK, -1);
    tscr_put(s, 0, 5, "When you log off the board, you are back in the directory.", TS_DK, -1);
    return 7 * 256;
}

static int draw_bye(tmenu_t *m, tscr *s)
{
    char b[96], c[12];
    clock_of(m, c);
    snprintf(b, sizeof b, "Thanks for calling Netstalgia Online. Time online: %s.", c);
    tscr_put(s, 0, 0, b, TS_W, -1);
    tscr_put(s, 0, 2, "NO CARRIER", TS_WH, -1);
    return 3 * 256;
}

/* ---- making the screen, sending it ------------------------------------ */

static void redraw(tmenu_t *m)
{
    tscr *s = &m->cur;
    int c = -1;
    tscr_clear(s);
    switch (m->screen) {
    case S_CONNECT: c = draw_connect(m, s); break;
    case S_MENU:    c = draw_menu(m, s); break;
    case S_PPP:     c = draw_ppp(m, s); break;
    case S_WHO:     c = draw_who(m, s); break;
    case S_OWN:     c = draw_own(m, s); break;
    case S_BBS:     c = draw_bbs(m, s); break;
    case S_BOARD:   c = draw_board(m, s); break;
    case S_DIAL:    c = draw_dial(m, s); break;
    default:        c = draw_bye(m, s); break;
    }
    if (c >= 0) { s->cy = c / 256; s->cx = c % 256; } else s->cx = -1;
    m->dirty = 1;
}

static void out_put(tmenu_t *m, const char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n && m->out_n < TM_OUT; i++) {
        m->out[(m->out_head + m->out_n) % TM_OUT] = p[i];
        m->out_n++;
    }
}

/* the screen as it is now, once the line has taken most of the last one */
static void emit(tmenu_t *m)
{
    size_t n;
    if (!m->dirty || m->out_n > DIR_BUDGET) return;
    n = m->full ? tscr_ansi(&m->cur, m->ansi, m->scratch, sizeof m->scratch)
                : tscr_diff(&m->sent, &m->cur, m->ansi, m->scratch, sizeof m->scratch);
    if (n >= sizeof m->scratch) n = sizeof m->scratch - 1;   /* (never: a screen is far less) */
    if (n > TM_OUT - m->out_n) return;    /* not yet: room first */
    out_put(m, m->scratch, n);
    if (!m->ansi && m->cfg.operator && m->screen == S_BBS) {   /* the prompt under the plain screen, and what is typed so far */
        out_put(m, "BBS: ", 5);
        out_put(m, m->q, strlen(m->q));
    }
    m->sent = m->cur;
    m->dirty = 0;
    m->full = 0;
}

static void show(tmenu_t *m, int screen)
{
    m->screen = screen;
    m->typed = 0;
    m->go_at = 0;
    redraw(m);
    if (screen == S_PPP) m->action = TM_ACT_PPP, m->act_wait = 1;
    else if (screen == S_BYE) m->action = TM_ACT_HANGUP, m->act_wait = 1;
    else if (screen == S_DIAL) m->action = TM_ACT_DIAL, m->act_board = m->board, m->act_wait = 1;
}

static void dir_search(tmenu_t *m)
{
    tdir_search(&m->view, m->q);
    m->dsel = m->dpage = 0;
    m->dirty_search = 1;
}

/* the same, done at once: a screen without colour is sent whole, so it
 * should not go out before its list is complete */
static void dir_search_now(tmenu_t *m)
{
    dir_search(m);
    (void)tdir_step(m->cfg.dir, &m->view, TDIR_MAX);
    m->dirty_search = 0;
}

static void open_item(tmenu_t *m, int i)
{
    int n = nitems(m);
    if (i >= n) { show(m, S_BYE); return; }
    switch (m->cfg.items[i].id) {
    case TM_PPP: show(m, S_PPP); break;
    case TM_WHO: show(m, S_WHO); break;
    case TM_BBS:
        if (!m->cfg.dir) return;
        if (m->view.nres == 0 && m->view.scanned == 0) dir_search(m);
        show(m, S_BBS);
        break;
    default: m->item_id = m->cfg.items[i].id; show(m, S_OWN); break;
    }
}

/* ---- keys ------------------------------------------------------------- */

static void key_menu(tmenu_t *m, int k)
{
    int n = nitems(m), i;
    if (k == K_UP || k == '-' || k == 5) { m->sel = (m->sel + n) % (n + 1); redraw(m); return; }
    if (k == K_DOWN || k == '+' || k == 24) { m->sel = (m->sel + 1) % (n + 1); redraw(m); return; }
    if (k == K_ENTER) { open_item(m, m->sel); return; }
    if (k == 'g' || k == 'G') { show(m, S_BYE); return; }
    for (i = 0; i < n; i++)
        if (k == m->cfg.items[i].key || (k >= 'a' && k <= 'z' && k - 32 == m->cfg.items[i].key)) {
            m->sel = i; m->typed = m->cfg.items[i].key; redraw(m);
            m->go_at = m->now + HOTKEY_MS; m->go_to = i;
            if (!m->go_at) m->go_at = 1;
            return;
        }
}

/* the operator's line changed: in colour the list follows at once; without
 * colour (every change is the whole screen) the key is echoed and the list
 * follows on Enter */
static void op_typed(tmenu_t *m, int k)
{
    if (m->ansi) { dir_search(m); redraw(m); return; }
    if (m->dirty) return;                          /* a screen on its way shows the line as it is */
    if (k == K_BS) out_put(m, "\b \b", 3);
    else { char ch = (char)k; out_put(m, &ch, 1); }
}

static void key_op(tmenu_t *m, int k)
{
    size_t l = strlen(m->q);
    int n, b;
    char low[TDIR_Q_MAX + 1];
    if (k == K_BS) { if (l) { m->q[l - 1] = '\0'; op_typed(m, k); } return; }
    if (k == K_ESC) {
        if (!l) { show(m, S_BYE); return; }
        m->q[0] = '\0'; dir_search_now(m); redraw(m);
        return;
    }
    if (k == K_ENTER) {
        b = tdir_find(m->cfg.dir, m->q);
        if (b >= 0) {                              /* a board's name or number: through at once */
            m->board = b;
            m->q[0] = '\0'; dir_search_now(m);       /* back from it: the whole list */
            show(m, S_DIAL);
            return;
        }
        for (l = 0; m->q[l]; l++) low[l] = m->q[l] >= 'A' && m->q[l] <= 'Z' ? (char)(m->q[l] + 32) : m->q[l];
        low[l] = '\0';
        if (strcmp(low, m->view.q) != 0) {        /* typed without colour: the list for it first */
            dir_search_now(m);
            redraw(m);
            return;
        }
        if (m->dirty_search) { (void)tdir_step(m->cfg.dir, &m->view, TDIR_MAX); m->dirty_search = 0; }
        if (m->view.nres) { m->board = m->view.res[m->dsel]; show(m, S_BOARD); }
        return;
    }
    if (k == '?') return;                          /* the old prompt's "search": typing is one already */
    if (k >= 32 && k < 127 && k != '+' && k != '<' && k != '>') {
        if (l < TDIR_Q_MAX && l < 40) { m->q[l] = (char)k; m->q[l + 1] = '\0'; op_typed(m, k); }
        return;
    }
    if (m->dirty_search) { (void)tdir_step(m->cfg.dir, &m->view, TDIR_MAX); m->dirty_search = 0; }
    n = m->view.nres;
    if (k == K_DOWN || k == '+' || k == 24) { if (m->dsel < n - 1) m->dsel++; }
    else if (k == K_UP || k == 5) { if (m->dsel > 0) m->dsel--; }
    else if (k == K_RIGHT || k == K_PGDN || k == '>' || k == 3) { int t = (m->dpage + 1) * TM_PER; m->dsel = t < n ? t : (n ? n - 1 : 0); }
    else if (k == K_LEFT || k == K_PGUP || k == '<' || k == 18) { int t = (m->dpage - 1) * TM_PER; m->dsel = t > 0 ? t : 0; }
    else return;
    m->dpage = m->dsel / TM_PER;
    redraw(m);
}

static void key_dir(tmenu_t *m, int k)
{
    int n;
    if (m->cfg.operator) { key_op(m, k); return; }
    if (m->searching) {
        size_t l = strlen(m->q);
        if (k == K_ENTER || k == K_ESC) { m->searching = 0; redraw(m); return; }
        if (k == K_BS) { if (l) m->q[l - 1] = '\0'; }
        else if (k >= 32 && k < 127 && l < TDIR_Q_MAX) { m->q[l] = (char)k; m->q[l + 1] = '\0'; }
        else return;
        dir_search(m);
        redraw(m);
        return;
    }
    /* a key that moves in the results wants all of them: a search still
     * going is finished now (rare: a poll or two finishes one) */
    if (m->dirty_search) { (void)tdir_step(m->cfg.dir, &m->view, TDIR_MAX); m->dirty_search = 0; }
    n = m->view.nres;
    if (k == K_ESC) {
        if (m->q[0]) { m->q[0] = '\0'; dir_search(m); redraw(m); }
        else show(m, S_MENU);
        return;
    }
    if (k == '/') { m->searching = 1; redraw(m); return; }
    if (k == K_DOWN || k == '+' || k == 24) { if (m->dsel < n - 1) m->dsel++; }
    else if (k == K_UP || k == '-' || k == 5) { if (m->dsel > 0) m->dsel--; }
    else if (k == K_RIGHT || k == K_PGDN || k == '>' || k == 3) { int t = (m->dpage + 1) * TM_PER; m->dsel = t < n ? t : (n ? n - 1 : 0); }
    else if (k == K_LEFT || k == K_PGUP || k == '<' || k == 18) { int t = (m->dpage - 1) * TM_PER; m->dsel = t > 0 ? t : 0; }
    else if (k == K_ENTER) { if (n) { m->board = m->view.res[m->dsel]; show(m, S_BOARD); } return; }
    else if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || k == '#') {
        int r = tdir_letter(m->cfg.dir, &m->view, (char)k);
        if (r >= 0) m->dsel = r;
    } else return;
    m->dpage = m->dsel / TM_PER;
    redraw(m);
}

static void key(tmenu_t *m, int k)
{
    if (m->action && m->act_wait) return;      /* on its way out: keys wait */
    switch (m->screen) {
    case S_CONNECT:
        if (k == 'y' || k == 'Y' || k == K_ENTER) m->ansi = 1;
        else if (k == 'n' || k == 'N') m->ansi = 0;
        else return;
        m->full = 1;
        if (m->cfg.operator && m->cfg.dir) { dir_search_now(m); show(m, S_BBS); }
        else show(m, S_MENU);
        return;
    case S_MENU:  key_menu(m, k); return;
    case S_BBS:   key_dir(m, k); return;
    case S_BOARD:
        if (k == 'c' || k == 'C') show(m, S_DIAL);
        else show(m, S_BBS);
        return;
    case S_PPP: case S_BYE: case S_DIAL: return;
    default: show(m, S_MENU); return;              /* Who's online, an owner's screen: any key */
    }
}

/* octets to keys: arrows and the paging keys of ANSI, VT100 and VT52 */
static void feed(tmenu_t *m, uint8_t c)
{
    if (m->esc_n) {
        m->esc[m->esc_n++] = c;
        if (m->esc_n == 2) {
            if (c == '[' || c == 'O') return;
            /* VT52: ESC A..D */
            m->esc_n = 0;
            if (c >= 'A' && c <= 'D') { key(m, c == 'A' ? K_UP : c == 'B' ? K_DOWN : c == 'C' ? K_RIGHT : K_LEFT); return; }
            key(m, K_ESC);
            feed(m, c);
            return;
        }
        if (c >= 0x40 && c <= 0x7E) {               /* the end of a sequence */
            int k = 0;
            if (c == 'A') k = K_UP; else if (c == 'B') k = K_DOWN; else if (c == 'C') k = K_RIGHT; else if (c == 'D') k = K_LEFT;
            else if (c == '~' && m->esc_n == 4 && m->esc[2] == '5') k = K_PGUP;
            else if (c == '~' && m->esc_n == 4 && m->esc[2] == '6') k = K_PGDN;
            m->esc_n = 0;
            if (k) key(m, k);
            return;
        }
        if (m->esc_n >= (int)sizeof m->esc) m->esc_n = 0;   /* too long to be anything we know */
        return;
    }
    if (c == 0x1B) { m->esc[0] = c; m->esc_n = 1; m->esc_at = m->now; return; }
    if (c == '\n') { if (!m->last_cr) key(m, K_ENTER); m->last_cr = 0; return; }
    m->last_cr = c == '\r';
    if (c == '\r') { key(m, K_ENTER); return; }
    if (c == 8 || c == 127) { key(m, K_BS); return; }
    if (c == 3 || c == 5 || c == 18 || c == 24) { key(m, c); return; }   /* the WordStar diamond */
    if (c >= 32 && c < 127) key(m, c);
}

/* ---- the interface ---------------------------------------------------- */

void tmenu_init(tmenu_t *m, const tmenu_cfg *cfg, uint32_t now_ms)
{
    memset(m, 0, sizeof *m);
    m->cfg = *cfg;
    m->now = m->t0 = now_ms;
    m->ansi = 1;                  /* the opening screen is in colour: it asks */
    m->full = 1;
    m->minute = 0;
    tscr_clear(&m->sent);
    show(m, S_CONNECT);
    emit(m);
}

void tmenu_input(tmenu_t *m, const uint8_t *p, size_t n, uint32_t now_ms)
{
    size_t i;
    m->now = now_ms;
    for (i = 0; i < n; i++) feed(m, p[i]);
    emit(m);
}

void tmenu_poll(tmenu_t *m, uint32_t now_ms)
{
    int minute;
    m->now = now_ms;
    if (m->esc_n == 1 && (int32_t)(now_ms - m->esc_at) >= ESC_MS) { m->esc_n = 0; key(m, K_ESC); }
    else if (m->esc_n > 1 && (int32_t)(now_ms - m->esc_at) >= 4 * ESC_MS) m->esc_n = 0;
    if (m->go_at && (int32_t)(now_ms - m->go_at) >= 0) { m->go_at = 0; open_item(m, m->go_to); }
    if (m->screen == S_OWN && m->not_ready && (int32_t)(now_ms - m->retry_at) >= 0) redraw(m);
    if (m->dirty_search && m->cfg.dir && tdir_step(m->cfg.dir, &m->view, SEARCH_STEP)) {
        m->dirty_search = 0;
        if (m->screen == S_BBS) redraw(m);
    }
    minute = (int)((now_ms - m->t0) / 60000u);
    if (minute != m->minute) {        /* the clocks in the bars */
        m->minute = minute;
        /* (without colour every change is the whole screen again: not for a clock) */
        if (m->ansi && m->screen != S_CONNECT && m->screen != S_DIAL && m->screen != S_BYE && !(m->action && m->act_wait))
            redraw(m);
    }
    emit(m);
}

size_t tmenu_output(tmenu_t *m, uint8_t *p, size_t cap)
{
    size_t n = 0;
    while (n < cap && m->out_n) {
        p[n++] = (uint8_t)m->out[m->out_head];
        m->out_head = (m->out_head + 1) % TM_OUT;
        m->out_n--;
    }
    emit(m);
    return n;
}

int tmenu_action(tmenu_t *m, int *board)
{
    int a = m->action;
    if (!a || m->out_n || m->dirty) return TM_ACT_NONE;
    m->action = TM_ACT_NONE;
    m->act_wait = 0;
    if (board) *board = m->act_board;
    return a;
}

void tmenu_redraw(tmenu_t *m)
{
    if (!(m->action && m->act_wait)) redraw(m);
    emit(m);
}

void tmenu_back(tmenu_t *m, uint32_t now_ms)
{
    m->now = now_ms;
    m->full = 1;                  /* the board's screens are on the caller's: all of ours again */
    show(m, S_BBS);
    emit(m);
}
