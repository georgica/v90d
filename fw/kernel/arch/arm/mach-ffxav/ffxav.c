// SPDX-License-Identifier: GPL-2.0
/*
 * Marvell FFxAV (88E7251) boards: the Obihai OBi200.
 *
 * Everything is described by the device tree; this file only names the
 * machine and restarts it. Restart is the watchdog with no time left, as
 * the board's own firmware does it: the chip-reset routing bits of the
 * system register at 0x8000f028 (bit 29 cleared, bit 28 set), then the
 * watchdog (0x8000d800) stopped, its period set to 0 and started.
 *
 * Written for the v90d project from the board's registers as its own
 * kernel uses them (fw/obi/README.md).
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#define FFXAV_SYS	0x8000f000
#define FFXAV_WDT	0x8000d800

static void ffxav_restart(enum reboot_mode mode, const char *cmd)
{
	void __iomem *sys = ioremap(FFXAV_SYS, 0x100);
	void __iomem *wdt = ioremap(FFXAV_WDT, 0x20);

	if (!sys || !wdt)
		return;
	writel((readl(sys + 0x28) & ~BIT(29)) | BIT(28), sys + 0x28);
	writel(readl(wdt) & ~3u, wdt);
	writel(0, wdt + 0x04);
	writel(readl(wdt) | 1, wdt);
	mdelay(1000);
}

/*
 * The megabyte at 0x03f00000, outside the kernel's memory: the bring-up
 * logs (the low-level debug output at 0x03f20000, debug/ffxav-ram.S) that
 * the next system to start reads after the watchdog's reset. Mapped for
 * good, uncached, where the low-level debug code expects it.
 */
static struct map_desc ffxav_io_desc[] __initdata = {
	{
		.virtual	= 0xfe800000,
		.pfn		= __phys_to_pfn(0x03f00000),
		.length		= SZ_1M,
		.type		= MT_DEVICE,
	},
};

static void __init ffxav_map_io(void)
{
	iotable_init(ffxav_io_desc, ARRAY_SIZE(ffxav_io_desc));
}

static const char *const ffxav_dt_compat[] __initconst = {
	"marvell,ffxav",
	NULL
};

DT_MACHINE_START(FFXAV_DT, "Marvell FFxAV (Obihai OBi200)")
	.dt_compat	= ffxav_dt_compat,
	.map_io		= ffxav_map_io,
	.restart	= ffxav_restart,
MACHINE_END
