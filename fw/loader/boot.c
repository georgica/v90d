/* See boot.h. */
#include "boot.h"

static const struct ld_board ld_boards[] = LD_BOARDS;

#define UIMAGE_HDR    64u
#define UIMAGE_MAGIC  0x27051956ul
#define GS_MAGIC      0x23C97AF9ul
#define FDT_MAGIC     0xD00DFEEDul
#define IMAGE_MAX     (LD_BASE_SIZE - LD_GS_HDR - UIMAGE_HDR)

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }

uint32_t ld_crc32(uint32_t crc, const uint8_t *p, uint32_t n)
{
    /* the reflected polynomial 0xEDB88320, four bits at a time */
    static const uint32_t t[16] = {
        0x00000000ul, 0x1DB71064ul, 0x3B6E20C8ul, 0x26D930ACul, 0x76DC4190ul, 0x6B6B51F4ul, 0x4DB26158ul, 0x5005713Cul,
        0xEDB88320ul, 0xF00F9344ul, 0xD6D6A3E8ul, 0xCB61B38Cul, 0x9B64C2B0ul, 0x86D3D2D4ul, 0xA00AE278ul, 0xBDBDF21Cul };
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        crc = t[crc & 15] ^ (crc >> 4);
        crc = t[crc & 15] ^ (crc >> 4);
    }
    return ~crc;
}

int ld_set_from_fdt(const uint8_t *fdt, uint32_t max)
{
    static const char key[] = "bootpartition=";
    const uint32_t klen = sizeof key - 1;
    uint32_t lim, i, k;
    if (!fdt || max < 8 || be32(fdt) != FDT_MAGIC) return -1;
    lim = be32(fdt + 4);
    if (lim > max) lim = max;
    /* the digit and the octet that ends the number must be inside too */
    for (i = 8; i + klen + 2 <= lim; i++) {
        for (k = 0; k < klen && fdt[i + k] == (uint8_t)key[k]; k++) ;
        if (k < klen) continue;
        if ((fdt[i + klen] == '0' || fdt[i + klen] == '1') && (fdt[i + klen + 1] == ' ' || fdt[i + klen + 1] == 0))
            return fdt[i + klen] - '0';
        return -1;
    }
    return -1;
}

int ld_tries(uint32_t w)
{
    int n = 0;
    for (w = ~w; w; w &= w - 1) n++;
    return n;
}

static uint32_t core_of(int set) { return set ? LD_CORE_B : LD_CORE_A; }
static uint32_t base_of(int set) { return set ? LD_BASE_B : LD_BASE_A; }

static int starts_with(const uint8_t *name, const char *s)
{
    for (; *s; s++, name++) if (*name != (uint8_t)*s) return 0;
    return 1;
}

/* a U-Boot image header whose own checksum is right */
static int uimage_hdr_ok(const uint8_t *h)
{
    uint8_t c[UIMAGE_HDR]; unsigned i;
    if (be32(h) != UIMAGE_MAGIC) return 0;
    for (i = 0; i < UIMAGE_HDR; i++) c[i] = h[i];
    c[4] = c[5] = c[6] = c[7] = 0;
    return ld_crc32(0, c, UIMAGE_HDR) == be32(h + 4);
}

/* does this core partition hold our loader? */
static int has_loader(qspi_t *q, uint32_t core, uint32_t *counter)
{
    uint8_t g[0x34], h[UIMAGE_HDR];
    if (qspi_read(q, core, g, sizeof g) != QSPI_OK || qspi_read(q, core + LD_GS_HDR, h, sizeof h) != QSPI_OK) return 0;
    *counter = le32(g + 0x30);
    return le32(g) == GS_MAGIC && uimage_hdr_ok(h) && starts_with(h + 32, LD_LOADER_NAME) && h[32 + sizeof LD_LOADER_NAME - 1] == 0;
}

/* our system image at flash offset `at`: copied to RAM and checked there */
static int load_image(qspi_t *q, uint32_t at, const ld_env *e, uint32_t *size)
{
    uint8_t h[UIMAGE_HDR]; uint32_t n;
    if (qspi_read(q, at, h, sizeof h) != QSPI_OK || !uimage_hdr_ok(h)) return 0;
    /* Linux, ARM, kernel, not compressed; built to run at LD_LOAD; ours */
    if (h[28] != 5 || h[29] != 2 || h[30] != 2 || h[31] != 0) return 0;
    if (be32(h + 16) != LD_LOAD || be32(h + 20) != LD_LOAD) return 0;
    if (!starts_with(h + 32, LD_SYSTEM_NAME) || starts_with(h + 32, LD_LOADER_NAME)) return 0;
    n = be32(h + 12);
    if (n == 0 || n > IMAGE_MAX || n > e->ram_max) return 0;
    if (qspi_read(q, at + UIMAGE_HDR, e->ram, n) != QSPI_OK) return 0;
    if (ld_crc32(0, e->ram, n) != be32(h + 24)) return 0;
    *size = n;
    return 1;
}

static int board_known(const uint8_t *name)
{
    unsigned i, k;
    for (i = 0; i < sizeof ld_boards / sizeof ld_boards[0]; i++) {
        const char *part = ld_boards[i].part;
        for (k = 0; part[k] && name[k] == (uint8_t)part[k]; k++) ;
        if (!part[k] && name[k] == 0) return 1;
    }
    return 0;
}

/* our own header stops being one (boot.h says why it is done this way) */
static void give_up(qspi_t *q, int may_write, uint32_t core)
{
    static const uint8_t zero[4] = { 0, 0, 0, 0 };
    uint8_t b[4];
    if (!may_write) return;
    if (qspi_write(q, core, zero, 4) == QSPI_OK && qspi_read(q, core, b, 4) == QSPI_OK && le32(b) != GS_MAGIC) return;
    (void)qspi_erase(q, core, QSPI_SECTOR);
}

/* --- a flash chip the driver does not know --------------------------------
 * The boot loader left it listening on one line, and every serial NOR chip
 * takes these three commands that way: write enable, erase 4 KiB, read
 * status. So our header's sector can still be erased, which is all it takes
 * to hand the box back to the stock firmware. The controller registers are
 * the ones fw/drv/qspi.c uses (refs/dvf99-core-facts.md section 7). */
enum { R_STS = 0x008, R_ISR = 0x00C, R_ICR = 0x014, R_FRD = 0x040, R_TRSTP = 0x300, R_CMD = 0x304, R_ADD = 0x308 };
static int raw_cmd(const qspi_io *io, uint32_t cmd, int with_addr, uint32_t addr, uint8_t *in, unsigned n)
{
    unsigned i;
    for (i = 0; ; i++) {
        uint32_t s = io->rd(io->ctx, R_STS);
        if (!(s & 0x80000000ul) && (s & 1)) break;
        if (i >= 100000u) return -1;
        io->udelay(io->ctx, 10);
    }
    io->wr(io->ctx, R_TRSTP, (1ul << 19) | ((uint32_t)n << 20) | (with_addr ? 1ul << 10 : 0));
    if (with_addr) io->wr(io->ctx, R_ADD, addr);
    io->wr(io->ctx, R_CMD, cmd | 0x80000000ul);
    for (i = 0; !(io->rd(io->ctx, R_ISR) & 0x40); i++) {
        if (i >= 100000u) return -1;
        io->udelay(io->ctx, 10);
    }
    io->wr(io->ctx, R_ICR, 0x40);
    for (i = 0; io->rd(io->ctx, R_ISR) & 0x40; i++) {
        if (i >= 100000u) return -1;
        io->udelay(io->ctx, 10);
    }
    io->wr(io->ctx, R_ICR, 0);
    for (i = 0; i < n; i++) {
        unsigned at = 31 - i;
        in[i] = (uint8_t)(io->rd(io->ctx, R_FRD + (at & ~3u)) >> (8 * (at & 3)));
    }
    return 0;
}
static int raw_status(const qspi_io *io, uint8_t mask, uint8_t want, unsigned polls)
{
    unsigned i;
    for (i = 0; ; i++) {
        uint8_t s;
        if (raw_cmd(io, 0x05ul | (5ul << 8) | (1ul << 13), 0, 0, &s, 1)) return -1;
        if ((s & mask) == want) return 0;
        if (i >= polls) return -1;
        io->udelay(io->ctx, 100);
    }
}
static void raw_erase_sector(const qspi_io *io, uint32_t off)
{
    uint8_t id[3];
    /* a chip still busy with what the driver last asked of it takes no
     * command but a status read: let it finish first (4 s at most) */
    (void)raw_status(io, 1, 0, 40000);
    if (raw_cmd(io, 0x9Ful | (9ul << 8) | (1ul << 13), 0, 0, id, 3)) return;
    if ((id[0] == 0 && id[1] == 0 && id[2] == 0) || (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF)) return;   /* nobody there */
    if (raw_cmd(io, 0x06ul | (9ul << 8), 0, 0, 0, 0) || raw_status(io, 2, 2, 1000)) return;
    if (raw_cmd(io, 0x20ul, 1, off, 0, 0)) return;
    (void)raw_status(io, 1, 0, 40000);
}

void ld_run(const ld_env *e, ld_result *r)
{
    qspi_t q;
    uint8_t b[16];
    uint32_t counter[2] = { 0, 0 }, core, state = 0xFFFFFFFFul, at[3];
    int tag[2], set, own, st, i, from[3];

    r->action = LD_RESET; r->why = LD_W_FLASH; r->image_set = -1; r->size = 0; r->tries = 0;
    r->set = set = ld_set_from_fdt(e->fdt, e->fdt_max);
    st = qspi_init(&q, &e->io);
    if (st != QSPI_OK) {
        /* A chip we do not know, or one (or a controller) that did not come
         * ready while being set up: either way hand the box back to stock,
         * or the stock boot loader starts this core again and again. The raw
         * erase reads the chip's ID first and does nothing if no chip
         * answers, so it cannot make things worse. */
        if (set >= 0) raw_erase_sector(&e->io, core_of(set));
        return;
    }

    /* Which set are we, and is its core really ours? Only then may we
     * write, and only our own header's sector and the state word's. */
    tag[0] = has_loader(&q, LD_CORE_A, &counter[0]);
    tag[1] = has_loader(&q, LD_CORE_B, &counter[1]);
    if (set < 0) {
        if (tag[0] && tag[1]) set = counter[1] > counter[0];
        else if (tag[0] || tag[1]) set = tag[1];
    }
    r->set = set;
    own = set >= 0 && tag[set];
    core = core_of(set > 0);
    if (own) {
        (void)qspi_allow(&q, core, core + QSPI_SECTOR);
        (void)qspi_allow(&q, core + LD_STATE_OFF, core + LD_STATE_OFF + QSPI_SECTOR);
    }

    if (qspi_read(&q, LD_BOARD_OFF, b, sizeof b) != QSPI_OK || !board_known(b)) {
        r->why = LD_W_BOARD; give_up(&q, own, core); return;
    }

    if (own) {
        if (qspi_read(&q, core + LD_STATE_OFF, b, 4) != QSPI_OK) return;
        state = le32(b);
        r->tries = ld_tries(state);
        if (r->tries >= LD_MAX_TRIES) { r->why = LD_W_TRIES; give_up(&q, own, core); return; }
    }

    /* our own set's base first, then the other set's; last the place the
     * bring-up build keeps its image (no stock header in front) */
    from[0] = set > 0;  at[0] = base_of(from[0]) + LD_GS_HDR;
    from[1] = !from[0]; at[1] = base_of(from[1]) + LD_GS_HDR;
    from[2] = 0;        at[2] = LD_BASE_A;
    for (i = 0; i < 3 && !load_image(&q, at[i], e, &r->size); i++) ;
    if (i == 3) { r->why = LD_W_NOIMAGE; give_up(&q, own, core); return; }
    r->image_set = from[i];

    if (own) {
        /* count this attempt before it is made: one more bit to zero */
        state &= state - 1;
        b[0] = (uint8_t)state; b[1] = (uint8_t)(state >> 8); b[2] = (uint8_t)(state >> 16); b[3] = (uint8_t)(state >> 24);
        if (qspi_write(&q, core + LD_STATE_OFF, b, 4) != QSPI_OK) { r->why = LD_W_RECORD; give_up(&q, own, core); return; }
        r->tries++;
    }
    r->action = LD_BOOT; r->why = LD_W_OK;
}
