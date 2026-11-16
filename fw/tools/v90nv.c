/* v90nv: what the v90d firmware keeps in flash, and how it gets there.
 *
 * On the running box (the flash is found by itself; -f FLASH names another
 * device or, for tests, a 16 MiB file that stands for the chip):
 *
 *   v90nv where            how this box starts our system and where things
 *                          are: NAME=VALUE lines (scheme, set, offsets, what
 *                          the next start will find)
 *   v90nv confirm          this start was a good one
 *   v90nv to-stock         the next start is the stock firmware again
 *   v90nv check FILE       is FILE a system image, or a firmware file with
 *                          one in it, that may be written to this box
 *   v90nv upgrade FILE     ... and write it where the next start looks
 *   v90nv conf-save FILE   keep FILE (the settings) in flash
 *   v90nv conf-load FILE   and get it back; exit 1 if none is kept
 *   v90nv serve-upgrade LEN [CONTENT-TYPE]
 *   v90nv serve-stock LEN  the same two actions as the answer to a web
 *                          request whose connection is our standard input
 *                          and output (nvhttp.c; started by the daemon)
 *
 * Exit: 0 done, 1 refused or nothing there (nothing was written), 2 trouble.
 * The two ways a box starts us, and what each action means in them, are in
 * DESIGN.md ("Starting from flash", "Release image") and nvboot.c.
 *
 * The older, plain commands (any MTD device or file, by name):
 *
 *   v90nv env-print DEV               U-Boot's saved environment, one per line
 *   v90nv env-get   DEV NAME          print one value; exit 1 if there is none
 *   v90nv env-set   DEV NAME [VALUE]  change (or without VALUE remove) one
 *                                     variable of an environment that is there
 *   v90nv env-write DEV LISTFILE      a new environment from NAME=VALUE lines
 *   v90nv blob-save DEV FILE          keep FILE (the settings) in flash
 *   v90nv blob-load DEV FILE          and get it back; exit 1 if none is kept
 *
 * DEV is an MTD character device (erased, written, read back) or, for tests
 * and for preparing images on the host, a plain file. Nothing is written
 * when the flash already holds what is asked for.
 *
 * Environment: as U-Boot's env_flash.c keeps it, 8 KiB: CRC-32 of the data
 * (little endian), then NAME=VALUE strings each ended by a zero octet and a
 * further zero octet after the last. env-set never creates an environment:
 * if the CRC is wrong it stops, so a boot script can not appear from
 * nothing.
 *
 * Blob: "V90D", length, CRC-32 (little endian), data.
 *
 * Written for the v90d project. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>
#include "nv.h"

#ifndef V90D_VERSION
#define V90D_VERSION "dev"
#endif
#define ENV_SIZE  8192u
#define ENV_DATA  (ENV_SIZE - 4)
#define BLOB_MAX  (60u * 1024)

static uint32_t crc32_(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFul; int k;
    while (n--) { c ^= *p++; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320ul & (0ul - (c & 1))); }
    return ~c;
}
static uint32_t le32(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void die(const char *what) { fprintf(stderr, "v90nv: %s: %s\n", what, strerror(errno)); exit(2); }

static int dev_read(const char *dev, uint8_t *buf, size_t n)
{
    int fd = open(dev, O_RDONLY); ssize_t r; size_t got = 0;
    if (fd < 0) return -1;
    while (got < n && (r = read(fd, buf + got, n - got)) > 0) got += (size_t)r;
    close(fd);
    memset(buf + got, 0xFF, n - got);       /* a short file reads as erased flash */
    return 0;
}

/* make the first n octets of dev equal buf: erase what must be erased,
 * write, read back. 0, or exits. */
static void dev_replace(const char *dev, const uint8_t *buf, size_t n)
{
    static uint8_t now[BLOB_MAX + ENV_SIZE];
    struct mtd_info_user mi; struct erase_info_user ei;
    int fd; size_t len = n;
    if (dev_read(dev, now, n) == 0 && memcmp(now, buf, n) == 0) return;
    fd = open(dev, O_RDWR | O_CREAT, 0600);
    if (fd < 0) die(dev);
    if (ioctl(fd, MEMGETINFO, &mi) == 0) {
        len = (n + mi.erasesize - 1) / mi.erasesize * mi.erasesize;
        if (len > mi.size) { fprintf(stderr, "v90nv: %s is too small\n", dev); exit(2); }
        ei.start = 0; ei.length = (uint32_t)len;
        if (ioctl(fd, MEMERASE, &ei)) die("erase");
    }
    if (pwrite(fd, buf, n, 0) != (ssize_t)n) die("write");
    if (fsync(fd) && errno != EINVAL && errno != EROFS) die("sync");
    if (pread(fd, now, n, 0) != (ssize_t)n || memcmp(now, buf, n)) { fprintf(stderr, "v90nv: %s does not read back what was written\n", dev); exit(2); }
    close(fd);
}

/* ---- environment ---- */
static uint8_t env[ENV_SIZE];

static int env_load(const char *dev)
{
    if (dev_read(dev, env, ENV_SIZE)) die(dev);
    return nv_env_ok(env) ? 0 : -1;
}
static const char *env_next(const char *p) { return nv_env_next(p); }
static const char *env_find(const char *name) { return nv_env_find(env, name); }
static void env_remove(const char *name) { nv_env_remove(env, name); }
static int env_add(const char *name, const char *value) { return nv_env_add(env, name, value); }
static void env_store(const char *dev)
{
    nv_env_seal(env);
    dev_replace(dev, env, ENV_SIZE);
}

/* ---- the box's own flash, by what is in it (nvboot.c) ---- */
static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb"); uint8_t *p = malloc(NV_FILE_MAX + 1);
    if (!f || !p) die(path);
    *n = fread(p, 1, NV_FILE_MAX + 1, f);
    fclose(f);
    return p;
}
static int result(int r, const char *msg)
{
    if (r == 0) { if (msg[0]) printf("v90nv: %s\n", msg); return 0; }
    if (r > 0) { fprintf(stderr, "v90nv: %s\n", msg[0] ? msg : "refused"); return 1; }
    fprintf(stderr, "v90nv: the flash did not do as told (%d)%s%s\n", r, msg[0] ? "; " : "", msg);
    return 2;
}
static int box_command(const char *flash, int argc, char **argv)
{
    static nvflash f; static nv_where w;
    const char *cmd = argv[0]; char msg[200] = ""; int r;
    if (!strncmp(cmd, "serve-", 6)) return nv_serve(cmd, flash, argc, argv);
    if (nf_open(&f, flash) != NV_OK) { fprintf(stderr, "v90nv: no flash to look at%s%s: %s\n", flash ? ": " : "", flash ? flash : "", strerror(errno)); return 2; }
    if (nv_locate(&f, &w, !strcmp(cmd, "where") || !strcmp(cmd, "to-stock")) != NV_OK) { fprintf(stderr, "v90nv: cannot read the flash\n"); return 2; }
    if (!strcmp(cmd, "where") && argc == 1) { nv_print_where(&w, V90D_VERSION); return 0; }
    if (!strcmp(cmd, "confirm") && argc == 1) return result(nv_confirm(&f, &w, msg, sizeof msg), msg);
    if (!strcmp(cmd, "to-stock") && argc == 1) return result(nv_to_stock(&f, &w, msg, sizeof msg), msg);
    if ((!strcmp(cmd, "check") || !strcmp(cmd, "upgrade")) && argc == 2) {
        const uint8_t *hdr, *img; size_t n, len; uint8_t *p = slurp(argv[1], &n);
        if (n > NV_FILE_MAX) { fprintf(stderr, "v90nv: %s is too large to be a firmware for this box\n", argv[1]); return 1; }
        r = nv_upgrade_check(&f, &w, p, n, &hdr, &img, &len, msg, sizeof msg);
        if (r || cmd[0] == 'c') return result(r, msg);
        return result(nv_upgrade_write(&f, &w, hdr, img, len, msg, sizeof msg), msg);
    }
    if (!strcmp(cmd, "conf-load") && argc == 2) { r = nv_conf_load(&f, &w, argv[1]); return r == 0 ? 0 : r == 1 ? 1 : 2; }
    if (!strcmp(cmd, "conf-save") && argc == 2) {
        r = nv_conf_save(&f, &w, argv[1]);
        if (r == NV_E_RANGE) fprintf(stderr, "v90nv: %s is larger than %u octets\n", argv[1], NV_BLOB_MAX);
        return r == 0 ? 0 : r == 1 ? 1 : 2;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *cmd, *dev, *flash = 0;
    static const char *const box[] = { "where", "confirm", "to-stock", "check", "upgrade", "conf-load", "conf-save", "serve-upgrade", "serve-stock" };
    unsigned i;
    if (argc > 2 && !strcmp(argv[1], "-f")) { flash = argv[2]; argv += 2; argc -= 2; }
    for (i = 0; argc > 1 && i < sizeof box / sizeof box[0]; i++)
        if (!strcmp(argv[1], box[i])) { int r = box_command(flash, argc - 1, argv + 1); if (r >= 0) return r; argc = 0; }
    cmd = argc > 2 ? argv[1] : ""; dev = argc > 2 ? argv[2] : "";
    if (!strcmp(cmd, "env-print") && argc == 3) {
        const char *p;
        if (env_load(dev)) { fprintf(stderr, "v90nv: no saved environment in %s (U-Boot uses its built-in one)\n", dev); return 1; }
        for (p = (const char *)env + 4; *p; p = env_next(p)) puts(p);
        return 0;
    }
    if (!strcmp(cmd, "env-get") && argc == 4) {
        const char *v;
        if (env_load(dev) || !(v = env_find(argv[3]))) return 1;
        puts(v);
        return 0;
    }
    if (!strcmp(cmd, "env-set") && (argc == 4 || argc == 5)) {
        if (env_load(dev)) { fprintf(stderr, "v90nv: no saved environment in %s: not creating one\n", dev); return 1; }
        {
            const char *old = env_find(argv[3]);
            if (argc == 5 ? (old && !strcmp(old, argv[4])) : !old) return 0;    /* as asked already */
        }
        env_remove(argv[3]);
        if (argc == 5 && env_add(argv[3], argv[4])) { fprintf(stderr, "v90nv: no room, or a bad name\n"); return 2; }
        env_store(dev);
        return 0;
    }
    if (!strcmp(cmd, "env-write") && argc == 4) {
        FILE *f = fopen(argv[3], "r"); static char line[4096];
        if (!f) die(argv[3]);
        memset(env, 0, sizeof env);
        while (fgets(line, sizeof line, f)) {
            char *eq; size_t k = strlen(line);
            if (k && line[k - 1] == '\n') line[--k] = 0; else if (k == sizeof line - 1) { fprintf(stderr, "v90nv: line too long\n"); return 2; }
            if (!k || line[0] == '#') continue;
            if (!(eq = strchr(line, '='))) { fprintf(stderr, "v90nv: not NAME=VALUE: %s\n", line); return 2; }
            *eq = 0;
            if (env_find(line)) { fprintf(stderr, "v90nv: %s given twice\n", line); return 2; }
            if (env_add(line, eq + 1)) { fprintf(stderr, "v90nv: no room, or a bad name: %s\n", line); return 2; }
        }
        fclose(f);
        env_store(dev);
        return 0;
    }
    if (!strcmp(cmd, "blob-save") && argc == 4) {
        static uint8_t b[12 + BLOB_MAX]; FILE *f = fopen(argv[3], "rb"); size_t n;
        if (!f) die(argv[3]);
        n = fread(b + 12, 1, BLOB_MAX, f);
        if (!feof(f)) { fprintf(stderr, "v90nv: %s is larger than %u octets\n", argv[3], BLOB_MAX); return 2; }
        fclose(f);
        memcpy(b, "V90D", 4); put32(b + 4, (uint32_t)n); put32(b + 8, crc32_(b + 12, n));
        dev_replace(dev, b, 12 + n);
        return 0;
    }
    if (!strcmp(cmd, "blob-load") && argc == 4) {
        static uint8_t b[12 + BLOB_MAX]; uint32_t n; FILE *f;
        if (dev_read(dev, b, sizeof b)) return 1;
        n = le32(b + 4);
        if (memcmp(b, "V90D", 4) || n > BLOB_MAX || le32(b + 8) != crc32_(b + 12, n)) return 1;
        if (!(f = fopen(argv[3], "wb"))) die(argv[3]);
        if (fwrite(b + 12, 1, n, f) != n || fclose(f)) die(argv[3]);
        return 0;
    }
    fprintf(stderr, "usage: v90nv [-f FLASH] where | confirm | to-stock | check FILE | upgrade FILE | conf-save FILE | conf-load FILE\n"
                    "       v90nv env-print DEV | env-get DEV NAME | env-set DEV NAME [VALUE] | env-write DEV LIST | blob-save DEV FILE | blob-load DEV FILE\n");
    return 2;
}
