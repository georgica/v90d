/* Update check: once a day, when the owner has turned it on, ask GitHub
 * whether a newer v90d release exists and, if so, remember its version and
 * download URL so the Firmware page can offer it. The asking is done by a
 * child process running a shell script (fw/updatecheck.sh) that follows the
 * releases/latest redirect; this module schedules it, compares versions, and
 * writes the result where the httpd reads it. Nothing is installed here: the
 * one-click install is a separate path (the Firmware page's /firmware/update).
 *
 * Runs in the daemon's loop, once per block, like bbslink. Daemon only. No
 * allocation after startup. */
#ifndef V90D_UPDATE_H
#define V90D_UPDATE_H
#include <stddef.h>
#include <stdint.h>

/* Turn the daily check on or off and tell it the running version (e.g.
 * "0.5.2"); called at startup and whenever the configuration changes. Off
 * means the box never contacts GitHub, and any remembered result is dropped. */
void update_config(int enabled, const char *running_version);

/* Once per line block. */
void update_poll(uint32_t now_ms);

/* If a newer release is on offer, put its version (e.g. "0.6.0") in
 * ver[0..cap) and return 1; otherwise ver is "" and it returns 0. Reads the
 * result file, so it is current; cheap enough for a page render. */
int update_available(char *ver, size_t cap);

/* The daemon ends: no child is left behind. */
void update_stop(void);

/* The command that prints "version=X.Y.Z" and "url=..." for the latest
 * release; "" = never check [sh /etc/updatecheck.sh]. And where the result
 * is kept for the httpd to read [/tmp/v90d.update]. For tests. */
void update_set_cmd(const char *cmd);
void update_set_state(const char *path);

/* Compare two dotted version strings ("0.5.2", "v0.6.0"): <0, 0 or >0 as a
 * is older, the same, or newer than b. A leading 'v' and missing trailing
 * components (treated as 0) are allowed. Exposed for the test. */
int update_ver_cmp(const char *a, const char *b);

#endif
