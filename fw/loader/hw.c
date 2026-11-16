/* v90d loader: the parts that only exist on the box. Register addresses and
 * sequences are the ones our kernel drivers use and that were measured on
 * the unit (HARDWARE.md); nothing here is tested on the host. */
#include "boot.h"

#define QSPI_REGS   0x05500000ul
#define QSPI_WINDOW 0x80000000ul
#define CMU         0x05300000ul
#define CMU_SWSYSRST 0x40
#define CMU_RESET0  0x50
#define RESET_QSPI  (1ul << 11)
#define RAM_LO      0xC8000000ul
#define RAM_HI      0xCA000000ul
extern char _start[];
void ld_jump(uint32_t entry) __attribute__((noreturn));
void ld_main(uint32_t r2, uint32_t moved);

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

/* A pause of at least `us` microseconds. There is no clock we want to
 * depend on this early: the loop (start.S) takes three cycles a turn at
 * 400 MHz with the instruction cache on, 133 turns a microsecond, and
 * anything slower only makes the wait longer. */
void ld_spin(uint32_t turns);
static void pause_us(unsigned us) { if (us) ld_spin((uint32_t)us * 150u); }

static uint32_t io_rd(void *ctx, unsigned reg) { (void)ctx; return REG32(QSPI_REGS + reg); }
static void io_wr(void *ctx, unsigned reg, uint32_t v) { (void)ctx; REG32(QSPI_REGS + reg) = v; }
static void io_win_read(void *ctx, uint32_t off, uint8_t *buf, uint32_t n)
{
    const volatile uint8_t *p = (const volatile uint8_t *)(QSPI_WINDOW + off);
    (void)ctx;
    if (!((off | (uintptr_t)buf) & 3)) {
        for (; n >= 4; n -= 4, p += 4, buf += 4) *(uint32_t *)(void *)buf = *(const volatile uint32_t *)(const volatile void *)p;
    }
    for (; n; n--) *buf++ = *p++;
}
/* a page program: the words go into the window one after the other. The
 * loader only ever programs one word at a time, so there is no stream that
 * could run dry. */
static void io_win_burst(void *ctx, uint32_t off, const uint32_t *w, unsigned n)
{
    volatile uint32_t *p = (volatile uint32_t *)(QSPI_WINDOW + off);
    (void)ctx;
    while (n--) *p++ = *w++;
}
static void io_udelay(void *ctx, unsigned us) { (void)ctx; pause_us(us); }
/* reset line 11 of the clock and reset unit is the flash controller */
static void io_reset(void *ctx)
{
    (void)ctx;
    REG32(CMU + CMU_RESET0) |= RESET_QSPI; pause_us(1000);
    REG32(CMU + CMU_RESET0) &= ~RESET_QSPI; pause_us(1000);
}

/* The watchdog is left alone. An earlier version started it here (100 s)
 * and fed it before the jump. Started from RAM out of the stock system, the
 * box then reset 2 to 3 s after every feed, in the loader itself or in the
 * kernel's first seconds, with the same values and pauses that the kernel's
 * driver uses for days (PROGRESS.md, "The loader on the unit"); the clock
 * the watchdog counts on is set up differently before our kernel has run,
 * and the state the stock boot loader leaves cannot be tried without making
 * the real start. So: no watchdog before the kernel, whose driver starts it
 * in its first half second. A hang in between is not caught by a reset; it
 * is caught by the count: every start is counted before it is made, and the
 * third power cycle gives the box back to the stock firmware. */

/* the whole box, as our kernel's restart does it */
static void box_reset(void)
{
    REG32(CMU + CMU_SWSYSRST) = 0x7496398Bul;
    REG32(CMU + CMU_SWSYSRST) = 0xB5C9C674ul;
    for (;;) ;
}

void ld_main(uint32_t r2, uint32_t moved)
{
    ld_env e;
    ld_result r;

    e.io.rd = io_rd; e.io.wr = io_wr; e.io.win_read = io_win_read; e.io.win_burst = io_win_burst;
    e.io.udelay = io_udelay; e.io.reset = io_reset; e.io.ctx = 0;
    /* r2 is the boot loader's device tree only if the boot loader started
     * us: then we run where its bootm put us */
    e.fdt = 0; e.fdt_max = 0;
    if (!moved && !(r2 & 3) && r2 >= RAM_LO && r2 < RAM_HI - 0x10000ul) {
        e.fdt = (const uint8_t *)r2; e.fdt_max = 0x10000ul;
    }
    e.ram = (uint8_t *)LD_LOAD;
    e.ram_max = (uint32_t)(uintptr_t)_start - LD_LOAD;       /* the image must end below us */

    ld_run(&e, &r);
#ifdef LD_TRACE
    /* bring-up only (make LD_EXTRA=-DLD_TRACE): what was decided is left in
     * the state word's sector, 0x40 in, where the next system can read it.
     * There is no console to say it on. */
    {
        qspi_t q;
        uint8_t t[8];
        uint32_t st = (r.set > 0 ? LD_CORE_B : LD_CORE_A) + LD_STATE_OFF;
        t[0] = 0xA5; t[1] = (uint8_t)r.action; t[2] = (uint8_t)r.why; t[3] = (uint8_t)r.tries;
        t[4] = (uint8_t)r.set; t[5] = (uint8_t)r.image_set; t[6] = (uint8_t)moved; t[7] = 0x5A;
        if (qspi_init(&q, &e.io) == QSPI_OK && qspi_allow(&q, st, st + QSPI_SECTOR) == QSPI_OK)
            (void)qspi_write(&q, st + 0x40, t, sizeof t);
    }
#endif
    if (r.action != LD_BOOT) box_reset();
    ld_jump(LD_LOAD);
}
