/* The network side of the bulletin board number (bbs.h): the list of
 * boards, kept up to date from the Telnet BBS Guide; the prompt a
 * caller's terminal program is shown; and the connection to the board,
 * which a child process makes (nc) while the telnet words are spoken here
 * (telnet.h). It runs in the daemon's loop, once per block, on non-blocking
 * descriptors; srv/main.c owns the per-port server objects and the log
 * (daemon.h). Daemon only. No allocation after startup. */
#ifndef V90D_BBSLINK_H
#define V90D_BBSLINK_H
#include <stddef.h>
#include <stdint.h>

/* octets from the caller's modem (an srv_raw_fn) */
void bbslink_from_modem(void *ctx, const uint8_t *oct, size_t n);
/* the BBS page's table (the httpd_on_bbs hook, status.h): the boards that
 * answer the search q as <tr> rows into buf[0..cap), after the first skip
 * of them and max at most; *shown of them, *left more follow */
size_t bbslink_rows(void *ctx, const char *q, int skip, int max, char *buf, size_t cap, int *shown, int *left);
/* once per line block */
void bbslink_poll(uint32_t now_ms);
/* the daemon ends: no child is left behind */
void bbslink_stop(void);

/* where things are. builtin: the list in the image [/etc/v90d.bbs]; live:
 * where a fetched list is kept, and live + ".new" while it is checked
 * [/tmp/v90d.bbs]; fetch: the shell command that prints the list, "" =
 * never fetch [sh /etc/bbsfetch.sh]; prog: run as PROG HOST PORT with the
 * connection on its standard input and output, instead of nc. */
void bbslink_set_builtin(const char *path);
void bbslink_set_live(const char *path);
void bbslink_set_fetch(const char *cmd);
void bbslink_set_prog(const char *prog);

#endif
