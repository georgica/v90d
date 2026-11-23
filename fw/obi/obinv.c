/* obinv: the Firmware page's flash tool on the Obihai OBi200 (the HT80x has
 * fw/tools/v90nv). Started by the daemon (-N) with the browser's connection
 * as standard input and output, as v90nv is (fw/tools/nvhttp.c):
 *
 *   obinv where                       how this box keeps its firmware, as
 *                                     NAME=VALUE lines for the page
 *   obinv check FILE                  would FILE be written, and what of it
 *   obinv serve-upgrade LEN [CTYPE]   the upload: check, answer, write, restart
 *   obinv serve-stock LEN             there is no switch to flip on this box:
 *                                     the answer says what to upload instead
 *
 * What it writes. An OBi firmware file (fw/obi/mkobifw.py has the format)
 * carries segments, each for a place in the flash. Ours and the maker's
 * differ only in /obi; a file for this box is written segment by segment,
 * only where the flash differs, and only these places: /obi, the root file
 * system, the kernel. A file whose boot loader or recovery images differ
 * from the box's is refused: those stay as they are, so the box's recovery
 * mode (which takes any firmware file) is always there. Then the file's
 * header goes to 0x470000, where the maker's updater keeps the header of the
 * last firmware written; the boot checks the flash against it (a header that
 * does not list every segment sends the box to recovery mode, found on the
 * unit). The power going while it writes leaves the box in recovery mode,
 * where the same file can be given again.
 *
 * The same file therefore takes the box back to the maker's application:
 * the owner's OBi firmware file (the one the v90d image was built from)
 * differs from the flash in /obi only.
 *
 * Exit: 0 done, 1 refused (nothing written), 2 trouble.
 * Written for the v90d project; no vendor code. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <mtd/mtd-user.h>
#include "ppp_md5.h"
#include "sbuf.h"
#include "page.h"

int page_consoles_tab;

#define FLASH_DEV   "/dev/mtd6"         /* "flash0": the whole 16 MiB chip */
#define FLASH_SIZE  0x1000000u
#define SECTOR      0x10000u
#define TABLE_AT    0x470000u           /* the header of the last firmware written */
#define HDR_LEN     0x400u
#define FILE_MAX    (16u << 20)
#define BODY_MAX    (FILE_MAX + 4096u)
#define SEG_MAX     7                   /* records 0xC0..0x280: the most a header holds */
#define RECV_S      20

static const char *const MAGIC = "OBi2XX_FW_PACK00";

/* the places a file may write; everything else must already be as the file has it */
static const struct { uint32_t type, at, size; const char *name; } PLACE[] = {
    { 3, 0xC00000, 0x240000, "/obi" },
    { 4, 0x480000, 0x6C0000, "root file system" },
    { 5, 0x050000, 0x280000, "kernel" },
};

typedef struct {
    uint32_t type, off, at, size;       /* size: the segment, its 64-octet header included */
    const uint8_t *payload;
    uint32_t len;                       /* payload octets */
    int place;                          /* index in PLACE, or -1: must match the flash */
    int differs;
} seg_t;

static const char *env_or(const char *n, const char *d) { const char *v = getenv(n); return v && v[0] ? v : d; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- the flash ---- */

static int flash_fd = -1;

static int flash_open(int wr)
{
    flash_fd = open(env_or("OBINV_FLASH", FLASH_DEV), (wr ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    return flash_fd < 0 ? -1 : 0;
}

static int flash_read(uint32_t at, uint8_t *p, uint32_t n)
{
    while (n) {
        ssize_t r = pread(flash_fd, p, n, at);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; at += (uint32_t)r; n -= (uint32_t)r;
    }
    return 0;
}

/* does the flash hold these octets at `at` */
static int flash_same(uint32_t at, const uint8_t *p, uint32_t n)
{
    static uint8_t buf[SECTOR];
    while (n) {
        uint32_t k = n < SECTOR ? n : SECTOR;
        if (flash_read(at, buf, k) || memcmp(buf, p, k)) return 0;
        at += k; p += k; n -= k;
    }
    return 1;
}

static int flash_erase(uint32_t at, uint32_t n)
{
    struct erase_info_user e;
    struct stat st;
    if (fstat(flash_fd, &st) == 0 && S_ISREG(st.st_mode)) {      /* a test file stands for the chip */
        static uint8_t ff[SECTOR];
        memset(ff, 0xff, sizeof ff);
        for (; n; at += SECTOR, n -= SECTOR) if (pwrite(flash_fd, ff, SECTOR, at) != SECTOR) return -1;
        return 0;
    }
    e.start = at; e.length = n;
    return ioctl(flash_fd, MEMERASE, &e);
}

/* erase the sectors under [at, at+n), write, read back */
static int flash_write(uint32_t at, const uint8_t *p, uint32_t n)
{
    uint32_t span = (n + SECTOR - 1) / SECTOR * SECTOR, done = 0;
    if (at % SECTOR || at + span > FLASH_SIZE) return -1;
    if (flash_erase(at, span)) return -1;
    while (done < n) {
        uint32_t k = n - done < SECTOR ? n - done : SECTOR;
        if (pwrite(flash_fd, p + done, k, at + done) != (ssize_t)k) return -1;
        done += k;
    }
    return flash_same(at, p, n) ? 0 : -1;
}

/* ---- the file ---- */

static void md5(const uint8_t *p, uint32_t n, uint8_t out[16])
{
    ppp_md5_t m;
    ppp_md5_init(&m); ppp_md5_update(&m, p, n); ppp_md5_final(&m, out);
}

/* 0 and the segments, or 1 and why not (nothing is written either way) */
static int parse(const uint8_t *f, size_t n, seg_t *s, int *count, char *why, size_t cap)
{
    uint32_t k, i, j;
    if (n < HDR_LEN || memcmp(f, MAGIC, 16)) { snprintf(why, cap, "it is not an OBi firmware file"); return 1; }
    if (le32(f + 0x84) != n) { snprintf(why, cap, "the file is not whole (%lu of %lu octets)", (unsigned long)n, (unsigned long)le32(f + 0x84)); return 1; }
    k = le32(f + 0x80);
    if (k == 0 || k > SEG_MAX) { snprintf(why, cap, "the file lists %lu parts", (unsigned long)k); return 1; }
    for (i = 0; i < k; i++) {
        const uint8_t *r = f + 0xC0 + i * 0x40, *h;
        uint8_t d[16];
        seg_t *g = &s[i];
        g->type = le32(r) & 0xffff;
        g->off = le32(r + 4); g->at = le32(r + 8); g->size = le32(r + 12);
        if (g->size < 0x40 || g->off < HDR_LEN || g->off > n || g->size > n - g->off) { snprintf(why, cap, "part %lu lies outside the file", (unsigned long)i); return 1; }
        h = f + g->off;
        if (memcmp(h, "\0\0\1\xba", 4) || le32(h + 0x3c) != g->size || memcmp(h + 16, r + 16, 16)) { snprintf(why, cap, "part %lu is damaged", (unsigned long)i); return 1; }
        /* the part's own header says the same as the file's record */
        if ((le32(h + 0x30) & 0xffff) != g->type || le32(h + 0x38) != g->at) { snprintf(why, cap, "part %lu is damaged (its header disagrees)", (unsigned long)i); return 1; }
        g->payload = h + 0x40; g->len = g->size - 0x40;
        md5(g->payload, g->len, d);
        if (memcmp(d, h + 16, 16)) { snprintf(why, cap, "part %lu is damaged (its checksum does not match)", (unsigned long)i); return 1; }
        if (g->at > FLASH_SIZE || g->len > FLASH_SIZE - g->at) { snprintf(why, cap, "part %lu does not fit the flash", (unsigned long)i); return 1; }
        g->place = -1;
        for (j = 0; j < sizeof PLACE / sizeof PLACE[0]; j++)
            if (PLACE[j].type == g->type && PLACE[j].at == g->at) {
                if (g->len > PLACE[j].size) { snprintf(why, cap, "the %s part is larger than its place", PLACE[j].name); return 1; }
                g->place = (int)j;
            }
        /* one part per place and per type: the boot would not take two */
        for (j = 0; j < i; j++)
            if (s[j].type == g->type || s[j].at == g->at) { snprintf(why, cap, "the file has two parts for one place (%lu and %lu)", (unsigned long)j, (unsigned long)i); return 1; }
    }
    /* the boot checks every part against this header: a file must carry them all */
    for (j = 0; j < sizeof PLACE / sizeof PLACE[0]; j++) {
        for (i = 0; i < k && s[i].place != (int)j; i++) {}
        if (i == k) { snprintf(why, cap, "the file has no %s part: only a whole firmware file can be written (mkobifw.py makes one)", PLACE[j].name); return 1; }
    }
    *count = (int)k;
    return 0;
}

/* compare with the flash: what would be written. 0 ok, 1 refused (why) */
static int plan(seg_t *s, int k, char *what, size_t cap, char *why, size_t wcap)
{
    int i, n = 0;
    what[0] = 0;
    for (i = 0; i < k; i++) {
        s[i].differs = !flash_same(s[i].at, s[i].payload, s[i].len);
        if (!s[i].differs) continue;
        if (s[i].place < 0) {
            snprintf(why, wcap, "its part of type %lu (the boot loader or a recovery image) differs from the box's; this page"
                     " never writes those. Use the box's recovery mode for such a file.", (unsigned long)s[i].type);
            return 1;
        }
        snprintf(what + strlen(what), cap - strlen(what), "%s%s", n++ ? ", " : "", PLACE[s[i].place].name);
    }
    if (!n) snprintf(what, cap, "nothing differs from the flash");
    return 0;
}

static int write_all(const uint8_t *f, seg_t *s, int k, char *msg, size_t cap)
{
    /* the whole sector of the header: whatever else it holds is kept */
    static uint8_t sec[SECTOR], hdr[HDR_LEN];
    const uint8_t *old = sec;
    uint32_t old_k;
    int i;
    if (flash_read(TABLE_AT, sec, SECTOR)) { snprintf(msg, cap, "the flash cannot be read"); return 2; }
    old_k = memcmp(old, MAGIC, 16) ? 0 : le32(old + 0x80);
    if (old_k > (0x280 - 0xC0) / 0x40) old_k = (0x280 - 0xC0) / 0x40;
    for (i = 0; i < k; i++) {
        if (!s[i].differs) continue;
        if (flash_write(s[i].at, s[i].payload, s[i].len)) { snprintf(msg, cap, "writing the %s failed", PLACE[s[i].place].name); return 2; }
    }
    /* the header, with the updater's "written" mark (octet 2 of each type):
     * set for what was written now, else as the box had it for that part */
    memcpy(hdr, f, HDR_LEN);
    for (i = 0; i < k; i++) {
        uint8_t *r = hdr + 0xC0 + i * 0x40;
        uint32_t j;
        int was = 0;
        for (j = 0; j < old_k; j++) {
            const uint8_t *o = old + 0xC0 + j * 0x40;
            if ((le32(o) & 0xffff) == s[i].type && le32(o + 8) == s[i].at && !memcmp(o + 16, r + 16, 16)) { was = o[2]; break; }
        }
        r[2] = s[i].differs ? 1 : (uint8_t)was;
    }
    if (memcmp(hdr, old, HDR_LEN)) {
        memcpy(sec, hdr, HDR_LEN);
        if (flash_write(TABLE_AT, sec, SECTOR)) { snprintf(msg, cap, "writing the firmware header failed"); return 2; }
    }
    return 0;
}

static void version_of(const uint8_t *h, char *out, size_t cap)
{
    size_t i;
    for (i = 0; i + 1 < cap && i < 0x40 && h[0x40 + i] >= 32 && h[0x40 + i] < 127; i++) out[i] = (char)h[0x40 + i];
    out[i] = 0;
}

/* ---- the page ---- */

static void send_all(const char *p, size_t n)
{
    while (n) {
        ssize_t r = write(1, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
        p += r; n -= (size_t)r;
    }
}

static void answer(int code, const char *badge, const char *text, const char *detail, int refresh_s)
{
    static char body[16384], head[640], meta[96];
    sbuf_t b, h;
    meta[0] = 0;
    if (refresh_s) snprintf(meta, sizeof meta, "<meta http-equiv=\"refresh\" content=\"%d;url=/firmware\">\n", refresh_s);
    sb_init(&b, body, sizeof body);
    page_top(&b, "firmware", meta, PAGE_TAB_FIRMWARE, NULL);
    sb_puts(&b, "<section class=\"hero\"><span class=\"b s1\">"); sb_puts(&b, badge);
    sb_puts(&b, "</span>\n<p class=\"sum\">"); sb_html(&b, text, 2000, 0); sb_puts(&b, "</p>\n");
    if (detail && detail[0]) { sb_puts(&b, "<p>"); sb_html(&b, detail, 2000, 0); sb_puts(&b, "</p>\n"); }
    sb_puts(&b, "<p><a href=\"/firmware\">Back to the Firmware page</a></p></section>\n" PAGE_END);
    if (sb_end(&b) < 0) return;
    sb_init(&h, head, sizeof head);
    sb_puts(&h, "HTTP/1.0 "); sb_u64(&h, (uint64_t)code);
    sb_puts(&h, code == 200 ? " OK" : code == 413 ? " Request Entity Too Large" : code == 409 ? " Conflict" :
                code >= 500 ? " Internal Server Error" : " Bad Request");
    sb_puts(&h, "\r\nServer: v90d\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "); sb_u64(&h, b.len);
    sb_puts(&h, "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'\r\n\r\n");
    if (sb_end(&h) < 0) return;
    send_all(head, h.len);
    send_all(body, b.len);
}

static void hang_up(void)
{
    char junk[512];
    struct timeval tv = { 1, 0 };
    int i;
    shutdown(1, SHUT_WR);
    setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (i = 0; i < 8 && read(0, junk, sizeof junk) > 0; i++) {}
    close(0); close(1);
}

static void note_state(const char *text)
{
    const char *path = env_or("V90NV_STATE", "/tmp/v90d.fwstate");
    char tmp[300];
    FILE *f;
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "%s\n", text);
    if (fclose(f) == 0) rename(tmp, path);
}

static int ask_restart(void)
{
    int fd = open(env_or("V90NV_RESTART_NOTE", "/tmp/v90d.restart"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static long read_body(uint8_t *p, size_t want)
{
    const char *e = getenv("V90NV_EARLY_FD");
    size_t got = 0;
    int fd = e ? atoi(e) : -1, fl;
    struct timeval tv = { RECV_S, 0 };
    if (fd > 2) {
        while (got < want) {
            ssize_t r = read(fd, p + got, want - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    fl = fcntl(0, F_GETFL);
    if (fl >= 0) fcntl(0, F_SETFL, fl & ~O_NONBLOCK);
    setsockopt(0, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (got < want) {
        ssize_t r = read(0, p + got, want - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (long)got;
}

static const uint8_t *find(const uint8_t *p, size_t n, const char *what, size_t k)
{
    size_t i;
    for (i = 0; i + k <= n; i++) if (p[i] == (uint8_t)what[0] && !memcmp(p + i, what, k)) return p + i;
    return 0;
}

/* the file inside a posted form: the part named "image", or the only one (as nvhttp.c) */
static uint8_t *form_file(uint8_t *p, size_t n, const char *ctype, size_t *len)
{
    char delim[80];
    const char *b = strstr(ctype, "boundary=");
    size_t k, dl, best_len = 0;
    const uint8_t *at, *end = p + n, *best = 0;
    int named = 0;
    if (!b) return 0;
    b += 9;
    if (*b == '"') { b++; k = strcspn(b, "\""); } else k = strcspn(b, "; \t");
    if (k == 0 || k > 70) return 0;
    snprintf(delim, sizeof delim, "\r\n--%.*s", (int)k, b);
    dl = k + 4;
    at = n >= dl - 2 && !memcmp(p, delim + 2, dl - 2) ? p + dl - 2 : find(p, n, delim, dl);
    if (at && at != p + dl - 2) at += dl;
    while (at && at + 2 <= end && !(at[0] == '-' && at[1] == '-')) {
        const uint8_t *hend = find(at, (size_t)(end - at), "\r\n\r\n", 4), *next;
        int is_image;
        if (!hend) return 0;
        next = find(hend + 4, (size_t)(end - hend - 4), delim, dl);
        if (!next) return 0;
        is_image = find(at, (size_t)(hend - at), "name=\"image\"", 12) != 0;
        if ((is_image && !named) || !best) { best = hend + 4; best_len = (size_t)(next - hend - 4); named = is_image; }
        at = next + dl;
    }
    if (!best) return 0;
    *len = best_len;
    return p + (best - p);
}

/* one writer at a time, even across a restart of the daemon */
static int lock_writer(void)
{
    int fd = open(env_or("OBINV_LOCK", "/tmp/obinv.lock"), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    return fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0 ? 0 : -1;
}

static int serve_upgrade(long want, const char *ctype)
{
    static seg_t s[SEG_MAX];
    uint8_t *p, *file;
    size_t n;
    int k, r;
    char why[300] = "", what[200], ver[80], line[600];

    if (want <= 0) { answer(400, "not written", "No file came with the request.", "", 0); return 1; }
    if (lock_writer()) { answer(409, "not written", "The flash is being written already. Wait until that is done.", "", 0); return 1; }
    if ((unsigned long)want > BODY_MAX) { answer(413, "not written", "The file is too large to be a firmware for this box.", "", 0); return 1; }
    if (!(p = malloc((size_t)want))) { answer(500, "not written", "Not enough memory to take the file.", "", 0); return 1; }
    if (read_body(p, (size_t)want) != want) { answer(400, "not written", "The upload broke off before the whole file had arrived.", "", 0); return 1; }
    file = p; n = (size_t)want;
    if (ctype && !strncmp(ctype, "multipart/form-data", 19)) {
        file = form_file(p, (size_t)want, ctype, &n);
        if (!file) { answer(400, "not written", "The upload is not a form this page understands.", "", 0); return 1; }
    }
    if (n == 0) { answer(400, "not written", "No file was chosen.", "", 0); return 1; }
    if (parse(file, n, s, &k, why, sizeof why)) { answer(400, "not written", "This file is not written to the box.", why, 0); return 1; }
    if (flash_open(1)) { answer(500, "not written", "The flash of the box cannot be opened.", "", 0); return 2; }
    if (plan(s, k, what, sizeof what, why, sizeof why)) { answer(400, "not written", "This file is not written to the box.", why, 0); return 1; }
    /* We run from the /obi part, and the writing may replace it (and the
     * system): every page of ours, the file included, stays in memory from
     * here on, and the restart at the end is ours, not a program's from the
     * flash. (Root on the box may lock all it likes; a test may not.) */
    (void)mlockall(MCL_CURRENT);
    version_of(file, ver, sizeof ver);
    snprintf(line, sizeof line, "The file is good: %s. Written to the flash now: %s.", ver, what);
    answer(200, "writing", line,
           "This takes a minute or two; then the box restarts by itself. Leave the power on. If the power does go"
           " while it writes, the box starts in its recovery mode, whose page takes the same file again. This page"
           " goes back to the Firmware page by itself.", 120);
    hang_up();
    snprintf(line, sizeof line, "writing %s: %s", ver, what);
    note_state(line);
    r = write_all(file, s, k, why, sizeof why);
    if (r) {
        snprintf(line, sizeof line, "FAILED: %s. The system that runs now is in memory and still whole: upload the file again"
                 " before the box is restarted (or give it to the recovery mode the box would start in).", why);
        note_state(line);
        return 2;
    }
    snprintf(line, sizeof line, "%s written (%s); the box restarts", ver, what);
    note_state(line);
    if (ask_restart()) note_state("written, but the restart could not be asked for: use the Restart button");
    if (!getenv("OBINV_NO_REBOOT")) {       /* tests run us on a file */
        sync();
        sleep(2);
        reboot(RB_AUTOBOOT);
    }
    return 0;
}

static int serve_stock(long want)
{
    uint8_t body[64];
    if (want > 0 && want < (long)sizeof body) (void)read_body(body, (size_t)want);
    answer(400, "not done", "This box has no stock firmware set aside to switch to.",
           "To go back to the maker's application, upload your OBi firmware file (the one the v90d image was built from)"
           " under \"Upload new firmware\": it differs from the flash in /obi only, and that is all that is written.", 0);
    return 1;
}

static int where(void)
{
    uint8_t h[HDR_LEN];
    char ver[80];
    if (flash_open(0) || flash_read(TABLE_AT, h, HDR_LEN)) return 2;
    printf("scheme=obi\nboard=Obihai OBi200\n");
    if (!memcmp(h, MAGIC, 16)) { version_of(h, ver, sizeof ver); printf("image_name=%s\n", ver); }
    return 0;
}

static int check(const char *path)
{
    static seg_t s[SEG_MAX];
    struct stat st;
    uint8_t *p;
    int fd, k;
    char why[300] = "", what[200], ver[80];
    if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, &st) || st.st_size > (off_t)FILE_MAX) { perror(path); return 2; }
    if (!(p = malloc((size_t)st.st_size)) || read(fd, p, (size_t)st.st_size) != st.st_size) return 2;
    if (parse(p, (size_t)st.st_size, s, &k, why, sizeof why)) { printf("refused: %s\n", why); return 1; }
    if (flash_open(0)) { perror("flash"); return 2; }
    if (plan(s, k, what, sizeof what, why, sizeof why)) { printf("refused: %s\n", why); return 1; }
    version_of(p, ver, sizeof ver);
    printf("good: %s; would write: %s\n", ver, what);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && !strcmp(argv[1], "where")) return where();
    if (argc == 3 && !strcmp(argv[1], "check")) return check(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "serve-upgrade")) return serve_upgrade(strtol(argv[2], 0, 10), argc > 3 ? argv[3] : 0);
    if (argc >= 3 && !strcmp(argv[1], "serve-stock")) return serve_stock(strtol(argv[2], 0, 10));
    fprintf(stderr, "usage: obinv where | check FILE | serve-upgrade LEN [CTYPE] | serve-stock LEN\n");
    return 2;
}
