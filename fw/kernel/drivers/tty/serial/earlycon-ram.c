// SPDX-License-Identifier: GPL-2.0
/*
 * An early console into memory, for a board nobody can read the serial
 * port of: earlycon=ramcon,ADDR. 64 KiB at ADDR (outside the kernel's
 * memory) get "RCON", the number of octets written, then the text; it
 * stops when they are full. Whoever starts the board next reads it (fw/obi:
 * the board's own firmware, through /dev/mem, after the watchdog has reset a
 * kernel that went wrong). Pair it with keep_bootcon to keep it after the
 * real consoles start.
 *
 * The earlycon core maps one page; the rest is mapped here with
 * early_ioremap, which stays mapped for as long as the console is used.
 *
 * Written for the v90d project.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/serial_core.h>
#include <asm/early_ioremap.h>

#define RCON_MAGIC	0x4e4f4352	/* "RCON" */
#define RCON_DATA	8
#define RCON_SIZE	0x10000

static void __iomem *rcon;

static void ramcon_write(struct console *con, const char *s, unsigned int n)
{
	u32 at = readl(rcon + 4);

	while (n-- && at < RCON_SIZE - RCON_DATA)
		writeb(*s++, rcon + RCON_DATA + at++);
	writel(at, rcon + 4);
}

static int __init ramcon_setup(struct earlycon_device *dev, const char *opt)
{
	if (!dev->port.mapbase)
		return -ENODEV;
	rcon = early_ioremap(dev->port.mapbase, RCON_SIZE);
	if (!rcon)
		rcon = dev->port.membase;	/* one page at least */
	if (!rcon)
		return -ENODEV;
	writel(RCON_MAGIC, rcon);
	writel(0, rcon + 4);
	dev->con->write = ramcon_write;
	return 0;
}
EARLYCON_DECLARE(ramcon, ramcon_setup);
