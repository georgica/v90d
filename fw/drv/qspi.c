/* See qspi.h. */
#include "qspi.h"

/* controller registers */
enum { R_CFG = 0x000, R_STS = 0x008, R_ISR = 0x00C, R_IER = 0x010, R_ICR = 0x014, R_FWD = 0x020, R_FRD = 0x040,
       R_RDTRSTP = 0x13C, R_RDCMD = 0x140, R_PPTRSTP = 0x144, R_PPCMD = 0x148, R_MEMADD = 0x154,
       R_TRSTP = 0x300, R_CMD = 0x304, R_ADD = 0x308, R_CSTIMING = 0x408 };
#define STS_BUSY    0x80000000ul
#define STS_IDLE    0x00000001ul
#define INT_DONE    0x40u               /* a register-driven transfer has ended */
#define TR_HAS_OP   (1ul << 19)
#define TR_ADDR3    (1ul << 10)
#define TR_QUAD     0x28Aul             /* opcode, address, data out and data in on four lines */
#define TR_SINGLE   0ul
#define CMD_START   0x80000000ul
#define CMD_IN      (1ul << 13)
#define CMD_RDID    (0x9Ful | (9ul << 8) | CMD_IN)
#define CMD_RDSR    (0x05ul | (5ul << 8) | CMD_IN)
#define CMD_WREN    (0x06ul | (9ul << 8))
#define CMD_WRDI    (0x04ul | (9ul << 8))
#define CMD_QPI     (0x38ul | (9ul << 8))
#define CMD_SE      (0x20ul | (0ul << 8))
#define SR_BUSY     0x01u
#define SR_WEL      0x02u
/* the values the unit's own software runs the chip with (read from it) */
#define V_CFG       0x00010009ul
#define V_CSTIMING  0x00000300ul
#define V_RDTRSTP   0x0008362Aul        /* fast read 0x0B, all on four lines */
#define V_RDCMD     0x0000000Bul
#define V_PPTRSTP   0x0008048Aul        /* page program 0x02, all on four lines */
#define V_PPCMD     0x00000002ul
#define V_MEMADD    0xFFFF0000ul

#define IDLE_POLLS  100000u             /* x 10 us: 1 s */
#define CHIP_POLLS  40000u              /* x 100 us: 4 s; a sector erase takes 0.4 s at most */
#define TRIES       4

#define RD(q, r)      ((q)->io.rd((q)->io.ctx, (r)))
#define WR(q, r, v)   ((q)->io.wr((q)->io.ctx, (r), (v)))

/* one register-driven command: opcode, perhaps an address, perhaps up to 32
 * octets in. The data buffers are filled from the end backwards. */
static int aux(qspi_t *q, uint32_t lines, uint32_t cmd, int with_addr, uint32_t addr, uint8_t *in, unsigned n)
{
    unsigned i;
    for (i = 0; ; i++) {
        uint32_t s = RD(q, R_STS);
        if (!(s & STS_BUSY) && (s & STS_IDLE)) break;
        if (i >= IDLE_POLLS) return QSPI_E_TIMEOUT;
        q->io.udelay(q->io.ctx, 10);
    }
    WR(q, R_TRSTP, lines | TR_HAS_OP | ((uint32_t)n << 20) | (with_addr ? TR_ADDR3 : 0));
    if (with_addr) WR(q, R_ADD, addr);
    WR(q, R_CMD, cmd | CMD_START);
    for (i = 0; !(RD(q, R_ISR) & INT_DONE); i++) {
        if (i >= IDLE_POLLS) return QSPI_E_TIMEOUT;
        q->io.udelay(q->io.ctx, 10);
    }
    WR(q, R_ICR, INT_DONE);             /* not self-clearing */
    for (i = 0; RD(q, R_ISR) & INT_DONE; i++) {
        if (i >= IDLE_POLLS) return QSPI_E_TIMEOUT;
        q->io.udelay(q->io.ctx, 10);
    }
    WR(q, R_ICR, 0);
    for (i = 0; i < n; i++) {
        unsigned at = 31 - i;
        in[i] = (uint8_t)(RD(q, R_FRD + (at & ~3u)) >> (8 * (at & 3)));
    }
    return QSPI_OK;
}

static int is_our_chip(qspi_t *q, uint32_t lines, int *yes)
{
    uint8_t id[3];
    int r = aux(q, lines, CMD_RDID, 0, 0, id, 3);
    *yes = r == QSPI_OK && id[0] == 0x1C && id[1] == 0x71 && id[2] == 0x18;     /* EN25QX128A */
    return r;
}

/* wait until the status register, under mask, reads want */
static int chip_wait(qspi_t *q, uint8_t mask, uint8_t want, unsigned polls)
{
    unsigned i;
    for (i = 0; ; i++) {
        uint8_t s;
        int r = aux(q, TR_QUAD, CMD_RDSR, 0, 0, &s, 1);
        if (r) return r;
        if ((s & mask) == want) return QSPI_OK;
        if (i >= polls) return QSPI_E_TIMEOUT;
        q->io.udelay(q->io.ctx, 100);
    }
}

static int write_enable(qspi_t *q)
{
    int r = aux(q, TR_QUAD, CMD_WREN, 0, 0, 0, 0);
    return r ? r : chip_wait(q, SR_WEL, SR_WEL, 1000);
}

/* everything the controller needs to know; the window settings are for the
 * chip in QPI and mean nothing before qspi_init() has put it there */
static void controller_setup(qspi_t *q)
{
    WR(q, R_IER, 0);                    /* polled */
    WR(q, R_CFG, V_CFG);
    WR(q, R_CSTIMING, V_CSTIMING);
    WR(q, R_RDTRSTP, V_RDTRSTP); WR(q, R_RDCMD, V_RDCMD);
    WR(q, R_PPTRSTP, V_PPTRSTP); WR(q, R_PPCMD, V_PPCMD);
    WR(q, R_MEMADD, V_MEMADD);
}

int qspi_init(qspi_t *q, const qspi_io *io)
{
    int r, yes;
    q->io = *io; q->ready = 0; q->nallow = 0; q->retries = 0;
    controller_setup(q);
    /* After U-Boot the chip listens on one line; after the stock system (a
     * RAM boot) on four. Asking in the wrong mode is heard as noise. */
    if ((r = is_our_chip(q, TR_SINGLE, &yes)) != QSPI_OK) return r;
    if (yes) {
        /* this part needs no enable bit for four-line operation: the
         * command alone switches it */
        if ((r = aux(q, TR_SINGLE, CMD_QPI, 0, 0, 0, 0)) != QSPI_OK) return r;
    }
    if ((r = is_our_chip(q, TR_QUAD, &yes)) != QSPI_OK) return r;
    if (!yes) return QSPI_E_ID;
    q->ready = 1;
    return QSPI_OK;
}

static int in_chip(uint32_t off, uint32_t n) { return n <= QSPI_SIZE && off <= QSPI_SIZE - n; }

static int allowed(const qspi_t *q, uint32_t off, uint32_t n)
{
    unsigned i;
    if (!in_chip(off, n)) return 0;
    for (i = 0; i < q->nallow; i++) if (off >= q->allow[i].lo && off + n <= q->allow[i].hi) return 1;
    return 0;
}

int qspi_allow(qspi_t *q, uint32_t lo, uint32_t hi)
{
    if (lo >= hi || hi > QSPI_SIZE || (lo % QSPI_SECTOR) || (hi % QSPI_SECTOR) || q->nallow >= QSPI_MAX_ALLOW) return QSPI_E_RANGE;
    q->allow[q->nallow].lo = lo; q->allow[q->nallow].hi = hi; q->nallow++;
    return QSPI_OK;
}

int qspi_read(qspi_t *q, uint32_t off, uint8_t *buf, uint32_t n)
{
    if (!q->ready) return QSPI_E_ID;
    if (!in_chip(off, n)) return QSPI_E_RANGE;
    q->io.win_read(q->io.ctx, off, buf, n);
    return QSPI_OK;
}

int qspi_erase(qspi_t *q, uint32_t off, uint32_t n)
{
    int r;
    if (!q->ready) return QSPI_E_ID;
    if (n == 0 || (off % QSPI_SECTOR) || (n % QSPI_SECTOR) || !allowed(q, off, n)) return QSPI_E_RANGE;
    for (; n; off += QSPI_SECTOR, n -= QSPI_SECTOR) {
        if ((r = write_enable(q)) != QSPI_OK) return r;
        if ((r = aux(q, TR_QUAD, CMD_SE, 1, off, 0, 0)) != QSPI_OK) return r;
        if ((r = chip_wait(q, SR_BUSY | SR_WEL, 0, CHIP_POLLS)) != QSPI_OK) return r;
    }
    return QSPI_OK;
}

/* one stretch inside a page: programmed as whole words, the octets around
 * ours as 0xFF, which leaves what is there as it is */
static int program(qspi_t *q, uint32_t off, const uint8_t *buf, unsigned n)
{
    uint32_t w[QSPI_PAGE / 4], a = off & ~3ul;
    uint8_t back[QSPI_PAGE];
    unsigned words = (unsigned)((off + n + 3 - a) / 4), i, t;
    int r;
    for (i = 0; i < words; i++) w[i] = 0xFFFFFFFFul;
    for (i = 0; i < n; i++) {
        unsigned at = (unsigned)(off - a) + i;
        w[at / 4] &= ~(0xFFul << (8 * (at & 3)));
        w[at / 4] |= (uint32_t)buf[i] << (8 * (at & 3));
    }
    for (t = 0; t < TRIES; t++) {
        if (t) {
            /* Seen on the unit: now and then (one page in a few hundred)
             * the chip never gets the program, and from then on the
             * controller programs nothing at all until it has been reset.
             * The chip keeps its mode through that. */
            q->retries++;
            q->io.reset(q->io.ctx);
            controller_setup(q);
        }
        if ((r = write_enable(q)) != QSPI_OK) return r;
        q->io.win_burst(q->io.ctx, a, w, words);
        /* Busy only: a burst the chip did not take as a program (seen on
         * the unit, about one page in 350) leaves the write enable latch
         * set for ever. The read-back below decides. */
        if ((r = chip_wait(q, SR_BUSY, 0, CHIP_POLLS)) != QSPI_OK) return r;
        q->io.win_read(q->io.ctx, off, back, n);
        for (i = 0; i < n && back[i] == buf[i]; i++) ;
        if (i == n) return QSPI_OK;
    }
    (void)aux(q, TR_QUAD, CMD_WRDI, 0, 0, 0, 0);       /* no latch left set behind us */
    return QSPI_E_VERIFY;
}

int qspi_write(qspi_t *q, uint32_t off, const uint8_t *buf, uint32_t n)
{
    if (!q->ready) return QSPI_E_ID;
    if (n == 0 || !allowed(q, off, n)) return QSPI_E_RANGE;
    while (n) {
        unsigned k = QSPI_PAGE - (unsigned)(off % QSPI_PAGE);
        int r;
        if (k > n) k = (unsigned)n;
        if ((r = program(q, off, buf, k)) != QSPI_OK) return r;
        off += k; buf += k; n -= k;
    }
    return QSPI_OK;
}
