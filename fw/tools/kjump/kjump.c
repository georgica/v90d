/* kjump: a bring-up tool, not part of the image. A module for the box's
 * STOCK kernel (3.4.20-rt31-dvf-v1.2.6.1-rc2, firmware 1.0.65) that leaves
 * that kernel and starts whatever was put at KJUMP_ADDR in RAM (our zImage,
 * loaded there with tools/peek -l). Nothing is written to flash: a reset
 * brings the stock firmware back.
 *
 * Built without the vendor kernel tree: the module record is laid out by
 * hand to the size and offsets measured on a vendor module of this firmware
 * (0x160 octets, name at 0x0c, init at 0xbc), and the one kernel function it
 * needs is called at the address the box's /proc/kallsyms gives. It fits
 * this firmware build only.
 */
#define KJUMP_ADDR	0xc9800000ul
#define SOFT_RESTART	0xd0009d5cul	/* T soft_restart */

static int kjump_init(void)
{
	void (*soft_restart)(unsigned long) = (void (*)(unsigned long))SOFT_RESTART;

	soft_restart(KJUMP_ADDR);	/* interrupts off, MMU off, jump */
	return 0;
}

struct this_module {
	char pad0[0x0c];
	char name[0xbc - 0x0c];
	int (*init)(void);
	char pad1[0x160 - 0xc0];
};

struct this_module __this_module
	__attribute__((section(".gnu.linkonce.this_module"), used)) = {
	.name = "kjump",
	.init = kjump_init,
};

static const char modinfo_vermagic[] __attribute__((section(".modinfo"), used)) =
	"vermagic=3.4.20-rt31-dvf-v1.2.6.1-rc2 preempt mod_unload ARMv5 p2v8 ";
static const char modinfo_license[] __attribute__((section(".modinfo"), used)) =
	"license=GPL";
