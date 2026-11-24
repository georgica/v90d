/* See nv.h. The two actions of the Firmware page that write the flash,
 * done here and not in the daemon: the daemon answers telephone lines
 * every 10 ms and must not wait for a megabyte from a browser or for a
 * flash chip. It checks who is asking, then starts us with the browser's
 * connection as our standard input and output and forgets about it.
 *
 *   v90nv serve-upgrade LEN [CONTENT-TYPE]   the body is the new firmware:
 *       a form upload (multipart/form-data, the field is called "image") or
 *       the file as it is
 *   v90nv serve-stock LEN                    the body is the confirmation
 *       form (confirm=stock)
 *
 * What of the body the daemon had already read comes first, through the
 * descriptor named in V90NV_EARLY_FD. The answer is a whole HTTP response
 * with the pages' common look (cfg/page.h). For an upgrade it is sent when
 * the file has been checked and before the flash is touched; how the write
 * went is then kept in a small file the Firmware page shows
 * (V90NV_STATE, /tmp/v90d.fwstate). When the flash is as asked a note is
 * left for the boot script, which restarts the box (V90NV_RESTART_NOTE,
 * /tmp/v90d.restart: the same note as the Restart button's). */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "nv.h"
#include "sbuf.h"
#include "page.h"

/* page.h: the pages this tool makes (while the flash is written) show the
 * tabs every box has */
int page_consoles_tab;

#define BODY_MAX (NV_FILE_MAX + 4096u)  /* a file of the largest size inside a form */
#define RECV_S   20                     /* a browser that sends nothing for this long is gone */

#include "fwserve.h"

static int serve_upgrade(const char *flash, long want, const char *ctype)
{
    static nvflash f; static nv_where w;
    const uint8_t *hdr, *img; size_t n, len; uint8_t *p, *file; char msg[200] = "", line[320]; int r;

    if (want <= 0) { answer(400, "not written", "No file came with the request.", "", 0); return 1; }
    if ((unsigned long)want > BODY_MAX) { answer(413, "not written", "The file is too large to be a firmware for this box.", "", 0); return 1; }
    if (!(p = malloc((size_t)want))) { answer(500, "not written", "Not enough memory to take the file.", "", 0); return 1; }
    if (read_body(p, (size_t)want) != want) { answer(400, "not written", "The upload broke off before the whole file had arrived.", "", 0); return 1; }
    file = p; n = (size_t)want;
    if (ctype && !strncmp(ctype, "multipart/form-data", 19)) {
        file = form_file(p, (size_t)want, ctype, &n);
        if (!file) { answer(400, "not written", "The upload is not a form this page understands.", "", 0); return 1; }
    }
    if (n == 0) { answer(400, "not written", "No file was chosen.", "", 0); return 1; }
    if (n > NV_FILE_MAX) { answer(413, "not written", "The file is too large to be a firmware for this box.", "", 0); return 1; }
    if (nf_open(&f, flash) != NV_OK || nv_locate(&f, &w, 0) != NV_OK) { answer(500, "not written", "The flash of the box cannot be read.", "", 0); return 2; }
    r = nv_upgrade_check(&f, &w, file, n, &hdr, &img, &len, msg, sizeof msg);
    if (r) { answer(400, "not written", "This file is not written to the box.", msg, 0); return 1; }

    snprintf(line, sizeof line, "The file is good: %s. It is being written to the flash now.", msg);
    answer(200, "writing", line,
           "This takes a few minutes; then the box restarts by itself with the new firmware. Leave the power on. "
           "If the power does go while it writes, the box starts the stock firmware, and the v90d firmware file has to be "
           "given to that again. This page goes back to the Firmware page by itself, which shows how the writing went "
           "for as long as the old system still answers.", 180);
    hang_up();
    snprintf(line, sizeof line, "writing %s", msg);
    note_state(line);
    r = nv_upgrade_write(&f, &w, hdr, img, len, msg, sizeof msg);
    if (r) {
        snprintf(line, sizeof line, "FAILED (%d): the new firmware is not in the flash, and the old one may be gone from it too. "
                 "The system that runs now is in memory and still whole: upload the file again before the box is restarted.", r);
        note_state(line);
        return 2;
    }
    snprintf(line, sizeof line, "%s; the box restarts", msg);
    note_state(line);
    if (ask_restart()) note_state("written, but the restart could not be asked for: use the Restart button");
    return 0;
}

static int serve_stock(const char *flash, long want)
{
    static nvflash f; static nv_where w;
    char body[64], msg[200] = ""; int r;
    if (want < 0 || want >= (long)sizeof body) { answer(400, "not done", "This is not the confirmation form.", "", 0); return 1; }
    if (read_body((uint8_t *)body, (size_t)want) != want) return 1;
    body[want] = 0;
    if (strcmp(body, "confirm=stock")) { answer(400, "not done", "Nothing was changed: the box was not ticked.", "", 0); return 1; }
    if (nf_open(&f, flash) != NV_OK || nv_locate(&f, &w, 1) != NV_OK) { answer(500, "not done", "The flash of the box cannot be read.", "", 0); return 2; }
    r = nv_to_stock(&f, &w, msg, sizeof msg);
    if (r > 0) { answer(409, "not done", "The box stays on this firmware.", msg, 0); return 1; }
    if (r < 0) { answer(500, "not done", "The flash did not take the change. The box stays as it is until it is restarted.", "", 0); return 2; }
    answer(200, "restarting", "The box is restarting into the stock firmware.",
           "It will answer at the address the stock firmware is set up for, with the stock firmware's own pages and password. "
           "To come back, give the v90d firmware file to its upgrade page.", 0);
    hang_up();
    note_state("back to the stock firmware; the box restarts");
    if (ask_restart()) return 2;
    return 0;
}

int nv_serve(const char *what, const char *flash, int argc, char **argv)
{
    long want;
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) return 2;
    want = strtol(argv[1], 0, 10);
    if (!strcmp(what, "serve-upgrade") && argc <= 3) return serve_upgrade(flash, want, argc == 3 ? argv[2] : 0);
    if (!strcmp(what, "serve-stock") && argc == 2) return serve_stock(flash, want);
    return 2;
}
