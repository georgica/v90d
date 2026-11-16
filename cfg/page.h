/* v90d/cfg - what the two HTML pages share: the style sheet and the frame
 * around the content (banner with the name, the row of tabs, the footer).
 * The web interface is called Nestaliga. No script, nothing fetched: the
 * server's Content-Security-Policy allows inline style only. Written for
 * this project; light and dark follow the browser. */
#ifndef V90D_CFG_PAGE_H
#define V90D_CFG_PAGE_H
#include "sbuf.h"

#define PAGE_NAME "Nestaliga"

/* the firmware's version: one place, fw/VERSION, handed in by the Makefiles
 * that build what goes into an image */
#ifndef V90D_VERSION
#define V90D_VERSION "dev"
#endif

/* the address the image always answers on besides its LAN address, so that
 * a wrong Ethernet setting cannot lock anybody out (fw/rcS sets it up) */
#define PAGE_RESCUE_IP "169.254.90.1"

#define PAGE_CSS \
    "<style>" \
    ":root{color-scheme:light dark;--bg:#eef1f5;--pn:#fff;--fg:#1b2230;--mu:#657084;" \
    "--ln:#dfe3ea;--lb:#f6f7fa;--hd:#1f2a44;--ac:#e8a33d;--ok:#1d8a55;--er:#c0392b;" \
    "--sh:0 1px 3px rgba(20,30,50,.08)}" \
    "@media(prefers-color-scheme:dark){:root{--bg:#0f131a;--pn:#182030;--fg:#e4e8ef;" \
    "--mu:#9aa5b8;--ln:#273044;--lb:#1c2536;--hd:#0b1019;--ok:#2f9e69;--er:#ff7b6b;--sh:none}}" \
    "*{box-sizing:border-box}" \
    "body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif}" \
    "header{background:var(--hd);color:#fff;border-bottom:3px solid var(--ac)}" \
    "header>div,main,footer{max-width:1000px;margin:0 auto;padding:0 16px}" \
    "h1{margin:0;padding:16px 0 12px;font-size:22px;letter-spacing:.18em;text-transform:uppercase}" \
    "h1 i{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--ac);" \
    "margin-right:10px}" \
    "h1 small{display:inline;margin-left:12px;font-size:12px;font-weight:400;letter-spacing:.04em;" \
    "text-transform:none;color:#c3cbda}" \
    "nav{display:flex;flex-wrap:wrap;gap:3px}" \
    "nav a,nav b{padding:8px 18px;font-size:12px;font-weight:700;letter-spacing:.08em;" \
    "text-transform:uppercase;text-decoration:none;border-radius:6px 6px 0 0}" \
    "nav a{color:#fff;background:rgba(255,255,255,.12)}" \
    "nav b{color:#1f2a44;background:var(--ac)}" \
    "nav .r{margin-left:auto;background:none;color:#c3cbda}" \
    "main{padding-top:18px}" \
    "section,.k{background:var(--pn);border:1px solid var(--ln);border-radius:10px;" \
    "box-shadow:var(--sh);overflow:hidden}" \
    "section{margin-bottom:16px}" \
    "h2{margin:0;padding:11px 14px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;" \
    "color:var(--mu);border-bottom:1px solid var(--ln)}" \
    "h2:before{content:'';display:inline-block;width:8px;height:8px;margin-right:8px;" \
    "border-radius:2px;background:var(--ac)}" \
    "table{border-collapse:collapse;width:100%}" \
    "th,td{padding:7px 14px;text-align:left;vertical-align:top;border-top:1px solid var(--ln)}" \
    "tr:first-child>*{border-top:0}" \
    "th{width:36%;background:var(--lb);color:var(--mu);font-weight:600}" \
    "td{font-variant-numeric:tabular-nums}" \
    ".g{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:0 16px}" \
    ".sw{margin:0 0 12px}.sw a,.sw b{display:inline-block;padding:5px 14px;border:1px solid var(--lb);border-radius:4px;font-size:13px;text-decoration:none}" \
    ".sw b{background:var(--ac);border-color:var(--ac);color:#fff}" \
    ".t{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px;" \
    "margin-bottom:16px}" \
    ".k{padding:12px 14px;color:var(--mu)}" \
    ".k small{margin:0 0 4px;font-size:11px;font-weight:700;letter-spacing:.08em;" \
    "text-transform:uppercase}" \
    ".k b{font-size:22px;color:var(--fg);font-variant-numeric:tabular-nums}" \
    ".m{height:6px;margin-top:8px;border-radius:3px;background:var(--lb);overflow:hidden}" \
    ".m i{display:block;height:100%;border-radius:3px;background:var(--ac)}" \
    ".x{overflow-x:auto}.x th{width:auto;white-space:nowrap}" \
    ".hero{padding:16px;border-left:4px solid var(--ac)}" \
    ".b{display:inline-block;padding:3px 14px;border-radius:99px;font-size:12px;font-weight:700;" \
    "letter-spacing:.06em;text-transform:uppercase;background:var(--lb);color:var(--mu);" \
    "border:1px solid var(--ln)}" \
    ".s1,.s2,.s4{background:var(--ac);border-color:var(--ac);color:#1f2a44}" \
    ".s3{background:var(--ok);border-color:var(--ok);color:#fff}" \
    ".sum{margin:8px 0 0;font-size:18px;font-weight:600;color:var(--fg)}" \
    "input,select{font:inherit;color:var(--fg);background:var(--pn);border:1px solid var(--ln);" \
    "border-radius:6px;padding:5px 8px;max-width:100%}" \
    "input[type=submit]{padding:9px 30px;font-weight:700;letter-spacing:.08em;" \
    "text-transform:uppercase;background:var(--ac);border-color:var(--ac);color:#1f2a44;cursor:pointer}" \
    "small{display:block;margin-top:2px;color:var(--mu)}" \
    "label{margin-right:14px;white-space:nowrap}" \
    "p{margin:0 0 16px}section>p{margin:10px 14px;color:var(--mu)}" \
    "main>p{padding:10px 14px;background:var(--pn);border:1px solid var(--ln);" \
    "border-left:4px solid var(--ac);border-radius:6px}" \
    ".lg{max-width:440px;margin:6vh auto}" \
    "section>p.err{color:var(--er);font-weight:600}" \
    "footer{padding-bottom:20px;font-size:12px;color:var(--mu)}" \
    "</style>\n"

/* The head and the frame down to the open <main>. title: after the name in
 * <title>; head: more lines for <head>; active: the tab of this page (the
 * index below, which for 1..5 is the settings page number of settings.h),
 * or, for a page outside the login, alone = the one tab it shows. */
#define PAGE_TAB_FIRMWARE 6
#define PAGE_TAB_CALLS 7
#define PAGE_TAB_CONSOLES 8
#define PAGE_TAB_BBS 9
/* The Dreamcast tab is shown only while a port has that service. Whoever
 * knows the settings says so before a page is made (the settings pages by
 * themselves, the server for the others); defined in status.c. */
extern int page_consoles_tab;   /* a port uses the dreamcast or saturn service */
static inline void page_top(sbuf_t *b, const char *title, const char *head, int active, const char *alone)
{
    static const struct { const char *href, *label; } tabs[] = {
        { "/", "Dashboard" }, { "/line", "Line" }, { "/modem", "Modem" }, { "/users", "Users" },
        { "/network", "Network" }, { "/system", "System" }, { "/firmware", "Firmware" },
        { "/calls", "Calls" }, { "/dreamcast", "Consoles" }, { "/bbs", "BBS" },
    };
    /* the order they are shown in */
    static const unsigned char order[] = { 0, 1, 2, 3, 4, 8, 9, 5, 6, 7 };
    int i, k;

    sb_puts(b, "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n");
    sb_puts(b, head);
    sb_puts(b, "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
               "<title>" PAGE_NAME " "); sb_puts(b, title);
    sb_puts(b, "</title>\n" PAGE_CSS "</head><body>\n"
               "<header><div><h1><i></i>" PAGE_NAME "<small>dial-up server</small></h1>\n<nav>");
    if (alone) {
        sb_puts(b, "<b>"); sb_puts(b, alone); sb_puts(b, "</b>");
    } else {
        for (k = 0; k < (int)sizeof order; k++) {
            i = order[k];
            if (i == PAGE_TAB_CONSOLES && !page_consoles_tab && i != active) continue;
            if (i == active) { sb_puts(b, "<b>"); sb_puts(b, tabs[i].label); sb_puts(b, "</b>"); }
            else {
                sb_puts(b, "<a href=\""); sb_puts(b, tabs[i].href); sb_puts(b, "\">");
                sb_puts(b, tabs[i].label); sb_puts(b, "</a>");
            }
        }
        sb_puts(b, "<a class=\"r\" href=\"/logout\">Log out</a>");
    }
    sb_puts(b, "</nav></div></header>\n<main>\n");
}

#define PAGE_END \
    "</main>\n<footer>" PAGE_NAME " &middot; v90d firmware " V90D_VERSION "</footer>\n</body></html>\n"

#endif
