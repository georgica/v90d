/* v90nv, the parts behind its commands (v90nv.c says what they are for).
 *
 *   nvflash.c   the flash chip as 16 MiB addressed by physical offset: an MTD
 *               device on the box, a plain file in the tests. Nothing is
 *               erased or programmed outside the ranges allowed first.
 *   nvimage.c   the formats: U-Boot image header, the stock partition header
 *               and firmware file (HARDWARE.md), U-Boot's environment.
 *   nvboot.c    where our things are in flash on this box, and the four
 *               actions on them: confirm, to-stock, upgrade, settings.
 *   nvhttp.c    the same two destructive actions as the answer to a web
 *               request the daemon hands over.
 *
 * The flash offsets are the loader's (fw/loader/boot.h), included, not
 * repeated. Written for the v90d project; no vendor code. */
#ifndef V90D_NV_H
#define V90D_NV_H
#include <stddef.h>
#include <stdint.h>
#include "boot.h"

#define NV_FLASH_SIZE 0x1000000ul
#define NV_SECTOR     4096u
#define NV_PAGE       256u
#define NV_CORE_SIZE  0x140000ul
#define NV_PROG_A     0x8E0000ul
#define NV_PROG_B     0xC1F000ul
#define NV_PROG_SIZE  0x33F000ul
#define NV_ENV        0xFBE000ul        /* U-Boot's saved environment */
#define NV_ENV_SIZE   8192u
#define NV_OLD_CONF   0x060000ul        /* settings of a box started the old way */
#define NV_CONF_OFF   0x20000ul         /* settings of a box started by the loader: in its own core */
#define NV_CONF_SIZE  0x10000ul
#define NV_BLOB_MAX   (60u * 1024)
#define NV_GS_MAGIC   0x23C97AF9ul
#define NV_UI_MAGIC   0x27051956ul
#define NV_UI_HDR     64u
#define NV_IMAGE_MAX  (LD_BASE_SIZE - LD_GS_HDR - NV_UI_HDR)
#define NV_FILE_MAX   0x400000ul        /* the largest file `upgrade` reads */
#define NV_BOARD      "ht802"           /* what an image names itself for: "v90d ht802 VERSION" */
#define NV_IMAGE_TAG  LD_SYSTEM_NAME " " NV_BOARD " "

enum { NV_OK = 0, NV_E_IO = -1, NV_E_RANGE = -2, NV_E_VERIFY = -3 };

/* ---- nvflash.c ---- */
typedef struct {
    int      fd, is_mtd, readonly;
    unsigned nallow;
    struct { uint32_t lo, hi; } allow[6];
    /* tests only, plain files only: stop the process before step number
     * `cut` (a step is one sector erase or one page program that changes
     * something); with `torn` that step is half done first */
    long     steps, cut;
    int      torn;
} nvflash;
/* path 0: $V90NV_FLASH, else the MTD partition named "all" */
int  nf_open(nvflash *f, const char *path);
void nf_close(nvflash *f);
int  nf_read(nvflash *f, uint32_t off, uint8_t *buf, uint32_t n);
int  nf_allow(nvflash *f, uint32_t lo, uint32_t hi);
int  nf_erase(nvflash *f, uint32_t off, uint32_t n);                        /* whole sectors; blank ones are left */
int  nf_program(nvflash *f, uint32_t off, const uint8_t *buf, uint32_t n);  /* onto erased flash, or clearing bits; read back */

/* ---- nvimage.c ---- */
uint32_t nv_crc32(const uint8_t *p, size_t n);
uint32_t nv_le32(const uint8_t *p);
uint32_t nv_be32(const uint8_t *p);
void     nv_put_le32(uint8_t *p, uint32_t v);
uint16_t nv_gs_sum(const uint8_t *p, size_t n);         /* 16-bit words added up */
/* a U-Boot image header whose own checksum is right */
int  nv_uimage_hdr_ok(const uint8_t *h);
/* p[0..n) is a system image of ours for this board, whole: 0, and *len is
 * header plus data (n may be longer: padding). Else the reason, as text. */
const char *nv_uimage_check(const uint8_t *p, size_t n, size_t *len);
/* p[0..n) is a firmware file in the stock format made by mkgsfw.py for
 * this board: deciphers its "base" in place. 0, with *hdr the 512-octet
 * partition header and *img, *len the system image in it (checked with
 * nv_uimage_check). Else the reason. */
const char *nv_container_open(uint8_t *p, size_t n, const uint8_t **hdr, const uint8_t **img, size_t *len);
void nv_aes128_decrypt_units(const uint8_t key[16], uint8_t *p, size_t n);  /* the format's CBC, 32-octet units */
void nv_stock_header_key(uint8_t key[16]);

/* U-Boot's environment in a buffer of NV_ENV_SIZE */
int         nv_env_ok(const uint8_t *env);
const char *nv_env_find(const uint8_t *env, const char *name);
const char *nv_env_next(const char *p);
void        nv_env_remove(uint8_t *env, const char *name);
int         nv_env_add(uint8_t *env, const char *name, const char *value);
void        nv_env_seal(uint8_t *env);                  /* zero the rest, set the CRC */

/* ---- nvboot.c ---- */
enum { NV_NONE = 0, NV_OLD, NV_LOADER };
typedef struct {
    int      scheme;
    int      set;               /* NV_LOADER: the set our loader is in, 0 = A, 1 = B; else -1 */
    int      board_ok;
    char     board[16];
    int      env_tries;         /* v90d_tries of a saved environment, -1 if there is none */
    int      old_bootable;      /* ... and an image of ours where its boot command looks */
    int      loader_tries;      /* starts the loader has counted and nobody confirmed */
    int      set_whole;         /* NV_LOADER: our prog is there and names our core and base */
    uint32_t core, state, conf, target;     /* target: where `upgrade` writes */
    uint32_t image;             /* where the next start finds its system image, 0 = nowhere */
    char     image_name[33];
    uint32_t image_len;
    int      stock_set;         /* the set the stock firmware would be in */
    int      stock_ok;          /* its three partitions pass the boot loader's test and are not ours */
} nv_where;
/* Also allows the ranges the scheme may write. deep: add up the stock set
 * and the image too (megabytes to read); without it stock_ok and image are
 * not filled in. */
int  nv_locate(nvflash *f, nv_where *w, int deep);
void nv_print_where(const nv_where *w, const char *version);
/* each: 0 done, 1 refused (msg says why, nothing written), <0 flash trouble */
int  nv_confirm(nvflash *f, const nv_where *w, char *msg, size_t cap);
int  nv_to_stock(nvflash *f, const nv_where *w, char *msg, size_t cap);
/* check p[0..n) (an image or a firmware file; it is changed in place) */
int  nv_upgrade_check(nvflash *f, const nv_where *w, uint8_t *p, size_t n, const uint8_t **hdr, const uint8_t **img, size_t *len,
                      char *msg, size_t cap);
int  nv_upgrade_write(nvflash *f, const nv_where *w, const uint8_t *hdr, const uint8_t *img, size_t len, char *msg, size_t cap);
int  nv_conf_load(nvflash *f, const nv_where *w, const char *path);
int  nv_conf_save(nvflash *f, const nv_where *w, const char *path);

/* ---- nvhttp.c ---- */
int  nv_serve(const char *what, const char *flash, int argc, char **argv);

#endif
