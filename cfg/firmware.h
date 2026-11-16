/* v90d/cfg - the Firmware page: which firmware runs and how the box starts
 * it, a form that uploads a new one, and the way back to the stock
 * firmware. Nothing here touches a socket, a file or the flash: the daemon
 * hands in two short texts, and the two actions are done by a helper
 * program the daemon starts (httpd.h, httpd_on_firmware).
 *
 * `where` is the output of `v90nv where` (fw/tools/v90nv.c): NAME=VALUE
 * lines, of which the page shows scheme, set, image_name, image_octets,
 * unconfirmed, old_tries, installed, settings, stock and board. `state` is
 * the one line the helper leaves about the last upload, or empty. Both may
 * be NULL or empty (a daemon that does not run on the box). All of it is
 * escaped. */
#ifndef V90D_CFG_FIRMWARE_H
#define V90D_CFG_FIRMWARE_H
#include <stddef.h>

#define FIRMWARE_HTML_MAX  16384  /* neither page ever needs more */
#define FIRMWARE_TEXT_MAX  1024   /* `where` and `state` are read up to here */

/* The page at /firmware. version: what this build calls itself.
 * can_write: the two forms are offered (there is somebody to hand them
 * to); they are also left out when `where` says scheme=none. Returns the
 * length, or -1 if cap is too small. */
int firmware_render_html(const char *version, const char *where, const char *state, int can_write, char *buf, size_t cap);

/* The page at /firmware/stock: what going back means, and the form that
 * does it (a box to tick, posted to the same path as confirm=stock). */
int firmware_render_stock(const char *where, char *buf, size_t cap);

/* The value of NAME in such lines: copied to out (cap octets, always
 * terminated), 1 if it is there. */
int firmware_value(const char *where, const char *name, char *out, size_t cap);

#endif
