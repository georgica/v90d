/* See nv.h. Where our things are in the flash of this box, and the actions
 * on them. Nothing here remembers how the box was started: every answer is
 * read from the flash, because every question is about the next start
 * (DESIGN.md, "Release image", says why that is enough). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv.h"

static const char *const boards[] = LD_BOARDS;
static const uint8_t GS_MAGIC_OCTETS[4] = { 0xF9, 0x7A, 0xC9, 0x23 };
static const uint8_t UI_MAGIC_OCTETS[4] = { 0x27, 0x05, 0x19, 0x56 };
static const uint8_t ZERO4[4] = { 0, 0, 0, 0 };

static uint32_t core_of(int set) { return set ? LD_CORE_B : LD_CORE_A; }
static uint32_t base_of(int set) { return set ? LD_BASE_B : LD_BASE_A; }
static uint32_t prog_of(int set) { return set ? NV_PROG_B : NV_PROG_A; }
static unsigned le16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
/* starts the loader has counted: one bit of the state word cleared for each (fw/loader/boot.c) */
static int tries_of(uint32_t w) { int n = 0; for (w = ~w; w; w &= w - 1) n++; return n; }

/* the stock boot loader's whole test of a partition: the signature, and the
 * payload adding up with the header's checksum. A length that runs out of
 * the partition counts as bad here (the boot loader itself would add up
 * whatever follows). */
static int gs_part_ok(nvflash *f, uint32_t off, uint32_t room, uint8_t h[LD_GS_HDR])
{
    static uint8_t buf[65536];
    uint32_t used, at, sum = 0;
    if (nf_read(f, off, h, LD_GS_HDR) != NV_OK || nv_le32(h) != NV_GS_MAGIC) return 0;
    used = nv_le32(h + 12) & ~1ul;
    if (used > room - LD_GS_HDR) return 0;
    for (at = 0; at < used; ) {
        uint32_t k = used - at > sizeof buf ? (uint32_t)sizeof buf : used - at;
        if (nf_read(f, off + LD_GS_HDR + at, buf, k) != NV_OK) return 0;
        sum += nv_gs_sum(buf, k);
        at += k;
    }
    return (uint16_t)(sum + le16(h + 0x12)) == 0;
}

/* does this core hold our loader? (the loader's own test of itself) */
static int has_loader(nvflash *f, uint32_t core, uint32_t *counter)
{
    uint8_t g[0x34], h[NV_UI_HDR];
    if (nf_read(f, core, g, sizeof g) != NV_OK || nf_read(f, core + LD_GS_HDR, h, sizeof h) != NV_OK) return 0;
    *counter = nv_le32(g + 0x30);
    return nv_le32(g) == NV_GS_MAGIC && nv_uimage_hdr_ok(h) && !strcmp((const char *)h + 32, LD_LOADER_NAME);
}

/* a system image of ours at `at`, as the loader (and U-Boot's bootm) would
 * take it: header and data both check */
static int image_at(nvflash *f, uint32_t at, char name[33], uint32_t *len)
{
    static uint8_t buf[65536];
    uint8_t h[NV_UI_HDR]; uint32_t n, done, crc = 0xFFFFFFFFul; int k;
    if (nf_read(f, at, h, sizeof h) != NV_OK || !nv_uimage_hdr_ok(h)) return 0;
    if (h[28] != 5 || h[29] != 2 || h[30] != 2 || h[31] != 0 || nv_be32(h + 16) != LD_LOAD || nv_be32(h + 20) != LD_LOAD) return 0;
    if (strncmp((const char *)h + 32, LD_SYSTEM_NAME, sizeof LD_SYSTEM_NAME - 1) || !strncmp((const char *)h + 32, LD_LOADER_NAME, sizeof LD_LOADER_NAME - 1)) return 0;
    n = nv_be32(h + 12);
    if (n == 0 || n > NV_IMAGE_MAX) return 0;
    for (done = 0; done < n; ) {
        uint32_t c = n - done > sizeof buf ? (uint32_t)sizeof buf : n - done, i;
        if (nf_read(f, at + NV_UI_HDR + done, buf, c) != NV_OK) return 0;
        for (i = 0; i < c; i++) { crc ^= buf[i]; for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320ul & (0ul - (crc & 1))); }
        done += c;
    }
    if (~crc != nv_be32(h + 24)) return 0;
    memcpy(name, h + 32, 32); name[32] = 0;
    for (k = 0; name[k]; k++) if (name[k] < 0x20 || name[k] > 0x7E) name[k] = '?';
    *len = n;
    return 1;
}

static uint8_t env[NV_ENV_SIZE];
static int env_tries(nvflash *f)
{
    const char *v;
    if (nf_read(f, NV_ENV, env, NV_ENV_SIZE) != NV_OK || !nv_env_ok(env)) return -1;
    v = nv_env_find(env, "v90d_tries");
    if (!v || v[0] < '0' || v[0] > '9' || v[1]) return -1;
    return v[0] - '0';
}

int nv_locate(nvflash *f, nv_where *w, int deep)
{
    uint8_t b[16], h[LD_GS_HDR], hc[LD_GS_HDR]; uint32_t counter[2] = { 0, 0 }, len; int tag[2], i; unsigned k; char name[33];
    memset(w, 0, sizeof *w);
    w->set = -1; w->stock_set = -1;
    if (nf_read(f, LD_BOARD_OFF, b, sizeof b) != NV_OK) return NV_E_IO;
    for (i = 0; i < 15 && b[i] >= 0x21 && b[i] <= 0x7E; i++) w->board[i] = (char)b[i];
    for (k = 0; k < sizeof boards / sizeof boards[0]; k++)
        if (b[i < 16 ? i : 15] == 0 && !strcmp(w->board, boards[k])) w->board_ok = 1;

    tag[0] = has_loader(f, LD_CORE_A, &counter[0]);
    tag[1] = has_loader(f, LD_CORE_B, &counter[1]);
    /* both: not something an upgrade can leave behind; the newer one, as
     * the stock boot loader would choose */
    if (tag[0] && tag[1]) w->set = counter[1] > counter[0];
    else if (tag[0] || tag[1]) w->set = tag[1];
    w->env_tries = env_tries(f);
    w->old_bootable = w->env_tries >= 0 && image_at(f, LD_BASE_A, name, &len);

    if (w->set >= 0) {
        w->scheme = NV_LOADER;
        w->core = core_of(w->set);
        w->state = w->core + LD_STATE_OFF;
        w->conf = w->core + NV_CONF_OFF;
        w->target = base_of(w->set);
        if (nf_read(f, w->state, b, 4) != NV_OK) return NV_E_IO;
        w->loader_tries = tries_of(nv_le32(b));
        w->set_whole = gs_part_ok(f, w->core, NV_CORE_SIZE, hc) && gs_part_ok(f, prog_of(w->set), NV_PROG_SIZE, h) &&
                       nv_le32(h + 0x28) == nv_le32(hc + 4);
        w->stock_set = !w->set;
        /* where the loader looks, in its order */
        if (!deep) ;
        else if (image_at(f, base_of(w->set) + LD_GS_HDR, w->image_name, &w->image_len)) w->image = base_of(w->set) + LD_GS_HDR;
        else if (image_at(f, base_of(!w->set) + LD_GS_HDR, w->image_name, &w->image_len)) w->image = base_of(!w->set) + LD_GS_HDR;
        else if (image_at(f, LD_BASE_A, w->image_name, &w->image_len)) w->image = LD_BASE_A;
        (void)nf_allow(f, w->core, w->core + NV_SECTOR);
        (void)nf_allow(f, w->state, w->state + NV_SECTOR);
        (void)nf_allow(f, w->conf, w->conf + NV_CONF_SIZE);
        (void)nf_allow(f, w->target, w->target + LD_BASE_SIZE);
        if (w->old_bootable) (void)nf_allow(f, NV_ENV, NV_ENV + NV_ENV_SIZE);
    } else if (w->env_tries >= 0) {
        w->scheme = NV_OLD;
        w->conf = NV_OLD_CONF;
        w->target = LD_BASE_A;
        w->stock_set = 1;
        if (deep && image_at(f, LD_BASE_A, w->image_name, &w->image_len)) w->image = LD_BASE_A;
        (void)nf_allow(f, w->conf, w->conf + NV_CONF_SIZE);
        (void)nf_allow(f, w->target, w->target + LD_BASE_SIZE);
        (void)nf_allow(f, NV_ENV, NV_ENV + NV_ENV_SIZE);
    }
    if (deep && w->stock_set >= 0) {
        uint32_t c;
        w->stock_ok = gs_part_ok(f, core_of(w->stock_set), NV_CORE_SIZE, h) && gs_part_ok(f, base_of(w->stock_set), LD_BASE_SIZE, h) &&
                      gs_part_ok(f, prog_of(w->stock_set), NV_PROG_SIZE, h) && !has_loader(f, core_of(w->stock_set), &c);
    }
    return NV_OK;
}

void nv_print_where(const nv_where *w, const char *version)
{
    static const char *const scheme[] = { "none", "old", "loader" };
    if (version) printf("version=%s\n", version);
    printf("scheme=%s\nset=%s\n", scheme[w->scheme], w->set < 0 ? "-" : w->set ? "B" : "A");
    printf("board=%s %s\n", w->board[0] ? w->board : "?", w->board_ok ? "known" : "not known");
    if (w->scheme == NV_LOADER) {
        printf("core=0x%06lx\nstate=0x%06lx\nunconfirmed=%d\n", (unsigned long)w->core, (unsigned long)w->state, w->loader_tries);
        printf("installed=%s\n", w->set_whole ? "whole" : "half");
    }
    if (w->env_tries >= 0) printf("old_tries=%d\n", w->env_tries);
    if (w->scheme != NV_NONE) {
        printf("settings=0x%06lx\nupgrade=0x%06lx\n", (unsigned long)w->conf, (unsigned long)w->target);
        if (w->image) printf("image=0x%06lx\nimage_name=%s\nimage_octets=%lu\n", (unsigned long)w->image, w->image_name, (unsigned long)w->image_len);
        else printf("image=none\n");
        printf("stock=%s %s\n", w->stock_set ? "B" : "A", w->stock_ok ? "good" : "damaged or missing");
    }
}

static void say(char *msg, size_t cap, const char *text) { if (cap) snprintf(msg, cap, "%s", text); }

/* tries < 0: the count is switched off. The variable gets a value that no
 * branch of the trial script matches ("x"), it is not taken away: the U-Boot
 * of this box takes `test "${v90d_tries}" = 1` for true when there is no
 * such variable, and would start our image once more (seen on the unit,
 * HARDWARE.md). env_tries() reads "x" as no count: no bench scheme. */
static int env_set_tries(nvflash *f, int tries)
{
    char v[2]; int r;
    if (nf_read(f, NV_ENV, env, NV_ENV_SIZE) != NV_OK || !nv_env_ok(env) || !nv_env_find(env, "v90d_tries")) return NV_E_IO;
    v[0] = tries >= 0 ? (char)('0' + tries) : 'x'; v[1] = 0;
    nv_env_remove(env, "v90d_tries");
    if (nv_env_add(env, "v90d_tries", v)) return NV_E_IO;
    nv_env_seal(env);
    /* the checksum goes in last: cut short, this is no environment at all
     * and U-Boot falls back on its built-in one, which starts the stock way */
    if ((r = nf_erase(f, NV_ENV, NV_ENV_SIZE)) != NV_OK) return r;
    if ((r = nf_program(f, NV_ENV + 4, env + 4, NV_ENV_SIZE - 4)) != NV_OK) return r;
    return nf_program(f, NV_ENV, env, 4);
}

int nv_confirm(nvflash *f, const nv_where *w, char *msg, size_t cap)
{
    int r;
    if (w->scheme == NV_NONE) { say(msg, cap, "not started from flash by a scheme we know: nothing to confirm"); return 1; }
    if (w->scheme == NV_LOADER && w->loader_tries) {
        /* as the loader wants it (fw/loader/boot.h): all ones again */
        if ((r = nf_erase(f, w->state, NV_SECTOR)) != NV_OK) return r;
    }
    if ((w->scheme == NV_OLD || w->old_bootable) && w->env_tries != 3)
        if ((r = env_set_tries(f, 3)) != NV_OK) return r;
    say(msg, cap, "start confirmed");
    return 0;
}

int nv_to_stock(nvflash *f, const nv_where *w, char *msg, size_t cap)
{
    uint8_t b[4]; int r;
    if (w->scheme == NV_NONE) { say(msg, cap, "not started from flash by a scheme we know: there is nothing to undo"); return 1; }
    if (!w->stock_ok) {
        if (cap) snprintf(msg, cap, "the stock firmware in set %s is damaged or missing: there is nothing to go back to", w->stock_set ? "B" : "A");
        return 1;
    }
    if (w->scheme == NV_LOADER) {
        /* what the loader does when it gives up: zeros programmed over the
         * signature of our core, never an erase first */
        r = nf_program(f, w->core, ZERO4, 4);
        if (r != NV_OK && (nf_read(f, w->core, b, 4) != NV_OK || nv_le32(b) == NV_GS_MAGIC))
            if ((r = nf_erase(f, w->core, NV_SECTOR)) != NV_OK) return r;
    }
    /* The bench scheme: the count is switched off (env_set_tries), not set
     * to 0. A 0 is also what a last, good start reads, and `confirm` (which
     * the boot script runs once more on the way down) would put it back to
     * 3; with no count there is no bench scheme and nothing to confirm. */
    if (w->scheme == NV_OLD || w->old_bootable)
        if ((r = env_set_tries(f, -1)) != NV_OK) return r;
    say(msg, cap, "the next start is the stock firmware");
    return 0;
}

int nv_upgrade_check(nvflash *f, const nv_where *w, uint8_t *p, size_t n, const uint8_t **hdr, const uint8_t **img, size_t *len,
                     char *msg, size_t cap)
{
    const char *why;
    (void)f;
    *hdr = 0;
    if (n >= 4 && nv_le32(p) == NV_GS_MAGIC) why = nv_container_open(p, n, hdr, img, len);
    else { why = nv_uimage_check(p, n, len); *img = p; }
    if (why) { say(msg, cap, why); return 1; }
    if (!w->board_ok) {
        if (cap) snprintf(msg, cap, "this box is a board this firmware has never run on (part number %s): nothing is written", w->board[0] ? w->board : "?");
        return 1;
    }
    if (w->scheme == NV_NONE) { say(msg, cap, "this system was not started from flash by a scheme we know: there is no place to write an image to"); return 1; }
    if (w->scheme == NV_LOADER && !w->set_whole) {
        say(msg, cap, "our set in flash is only half installed: put the firmware file through the stock firmware's upgrade page again");
        return 1;
    }
    if (cap) snprintf(msg, cap, "%s, %lu octets", (const char *)*img + 32, (unsigned long)*len);
    return 0;
}

int nv_upgrade_write(nvflash *f, const nv_where *w, const uint8_t *hdr, const uint8_t *img, size_t len, char *msg, size_t cap)
{
    uint8_t *buf, *back, cur[LD_GS_HDR], core[LD_GS_HDR], prog[LD_GS_HDR], b[4];
    uint32_t t = w->target, tlen, magic2 = 0, first, k;
    const uint8_t *m1; int r = NV_E_IO;

    if (w->scheme == NV_NONE || len < NV_UI_HDR || len > NV_UI_HDR + NV_IMAGE_MAX) return NV_E_RANGE;
    buf = calloc(1, LD_BASE_SIZE); back = malloc(LD_BASE_SIZE);
    if (!buf || !back) { free(buf); free(back); return NV_E_IO; }
    if (w->scheme == NV_LOADER) {
        uint32_t used = (uint32_t)((len + 31) & ~(size_t)31), counter;
        /* The stock boot loader starts our set as long as all three parts
         * add up and "prog" names the versions of the other two. So the
         * new base keeps the version prog names, whatever the file calls
         * itself, and the install counter of the set. */
        if (nf_read(f, t, cur, LD_GS_HDR) != NV_OK || nf_read(f, w->core, core, LD_GS_HDR) != NV_OK ||
            nf_read(f, prog_of(w->set), prog, LD_GS_HDR) != NV_OK) goto out;
        counter = nv_le32(core + 0x30);
        if (nv_le32(cur) == NV_GS_MAGIC) { if (nv_le32(cur + 0x30) > counter) counter = nv_le32(cur + 0x30); }
        memcpy(buf, hdr ? hdr : nv_le32(cur) == NV_GS_MAGIC ? cur : core, LD_GS_HDR);
        memcpy(buf + LD_GS_HDR, img, len);
        nv_put_le32(buf, NV_GS_MAGIC);
        nv_put_le32(buf + 4, nv_le32(prog + 0x2C));
        nv_put_le32(buf + 8, LD_BASE_SIZE);
        nv_put_le32(buf + 12, used);
        buf[0x10] = 0x0D; buf[0x11] = 0;
        k = (0x10000ul - nv_gs_sum(buf + LD_GS_HDR, used)) & 0xFFFF;
        buf[0x12] = (uint8_t)k; buf[0x13] = (uint8_t)(k >> 8);
        nv_put_le32(buf + 0x30, counter);
        tlen = LD_GS_HDR + used;
        m1 = GS_MAGIC_OCTETS; magic2 = LD_GS_HDR;
    } else {
        memcpy(buf, img, len);
        tlen = (uint32_t)len;
        m1 = UI_MAGIC_OCTETS;
    }
    if (nf_read(f, t, back, tlen) != NV_OK) goto out;
    if (!memcmp(back, buf, tlen)) { say(msg, cap, "this image is already there: nothing written"); r = 0; goto out; }

    /* 1. what is there stops being an image: zeros over its signatures.
     * From here to the last step a start of the box finds no image of ours
     * and takes the stock firmware; the system that runs is in RAM and can
     * simply do this again. */
    for (k = 0; k <= LD_GS_HDR; k += LD_GS_HDR) {
        if (nf_read(f, t + k, b, 4) != NV_OK) goto out;
        if (!memcmp(b, GS_MAGIC_OCTETS, 4) || !memcmp(b, UI_MAGIC_OCTETS, 4))
            if ((r = nf_program(f, t + k, ZERO4, 4)) != NV_OK) goto out;
    }
    /* 2. everything behind the first sector */
    first = tlen < NV_SECTOR ? tlen : NV_SECTOR;
    if (tlen > NV_SECTOR) {
        uint32_t end = (tlen + NV_SECTOR - 1) & ~(uint32_t)(NV_SECTOR - 1);
        if ((r = nf_erase(f, t + NV_SECTOR, end - NV_SECTOR)) != NV_OK) goto out;
        if ((r = nf_program(f, t + NV_SECTOR, buf + NV_SECTOR, tlen - NV_SECTOR)) != NV_OK) goto out;
    }
    /* 3. the first sector, without the signatures */
    memcpy(back, buf, first);
    memset(back, 0xFF, 4);
    if (magic2) memset(back + magic2, 0xFF, 4);
    if ((r = nf_erase(f, t, NV_SECTOR)) != NV_OK) goto out;
    if ((r = nf_program(f, t, back, first)) != NV_OK) goto out;
    /* 4. all of it once more, as the next start will read it */
    memset(buf, 0xFF, 4);
    if (magic2) memset(buf + magic2, 0xFF, 4);
    r = NV_E_VERIFY;
    if (nf_read(f, t, back, tlen) != NV_OK || memcmp(back, buf, tlen)) goto out;
    /* 5. the signatures: the inner one, then the one the boot loader goes by */
    if (magic2 && (r = nf_program(f, t + magic2, UI_MAGIC_OCTETS, 4)) != NV_OK) goto out;
    if ((r = nf_program(f, t, m1, 4)) != NV_OK) goto out;
    if (cap) snprintf(msg, cap, "written: %s, %lu octets at 0x%06lx", (const char *)img + 32, (unsigned long)len, (unsigned long)(t + magic2));
    r = 0;
out:
    free(buf); free(back);
    return r;
}

/* ---- the settings: "V90D", length, CRC-32 (little endian), data ---- */
static uint8_t blob[12 + NV_BLOB_MAX], blob_now[12 + NV_BLOB_MAX];

int nv_conf_load(nvflash *f, const nv_where *w, const char *path)
{
    uint32_t n; FILE *o;
    if (w->scheme == NV_NONE) return 1;
    if (nf_read(f, w->conf, blob, sizeof blob) != NV_OK) return NV_E_IO;
    n = nv_le32(blob + 4);
    if (memcmp(blob, "V90D", 4) || n > NV_BLOB_MAX || nv_le32(blob + 8) != nv_crc32(blob + 12, n)) return 1;
    if (!(o = fopen(path, "wb"))) return NV_E_IO;
    if (fwrite(blob + 12, 1, n, o) != n) { fclose(o); return NV_E_IO; }
    return fclose(o) ? NV_E_IO : 0;
}

int nv_conf_save(nvflash *f, const nv_where *w, const char *path)
{
    FILE *i; size_t n; uint32_t total; int r;
    if (w->scheme == NV_NONE) return 1;
    if (!(i = fopen(path, "rb"))) return NV_E_IO;
    n = fread(blob + 12, 1, NV_BLOB_MAX, i);
    if (!feof(i)) { fclose(i); return NV_E_RANGE; }
    fclose(i);
    memcpy(blob, "V90D", 4); nv_put_le32(blob + 4, (uint32_t)n); nv_put_le32(blob + 8, nv_crc32(blob + 12, n));
    total = 12 + (uint32_t)n;
    if (nf_read(f, w->conf, blob_now, total) != NV_OK) return NV_E_IO;
    if (!memcmp(blob_now, blob, total)) return 0;
    /* the old one stops being one, the new one becomes one with its last write */
    if (!memcmp(blob_now, "V90D", 4) && (r = nf_program(f, w->conf, ZERO4, 4)) != NV_OK) return r;
    if ((r = nf_erase(f, w->conf, (total + NV_SECTOR - 1) & ~(uint32_t)(NV_SECTOR - 1))) != NV_OK) return r;
    if ((r = nf_program(f, w->conf + 4, blob + 4, total - 4)) != NV_OK) return r;
    return nf_program(f, w->conf, blob, 4);
}
