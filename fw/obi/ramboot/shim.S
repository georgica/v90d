@ The boot shim in front of a zImage RAM-booted by kjump on the OBi200.
@ It runs at 0x03800000, MMU and caches off, interrupts off, then falls into
@ the zImage that follows it (position independent; its device tree is
@ appended). On the way: the watchdog to its longest period (1.6 s) and fed
@ once, so the kernel has time to start its own feeding (timer-ffxav.c);
@ a mark that the shim ran (0x03f01000: "SHIM"). Linux: r0 0, r1 ~0 (a
@ device tree, not a machine number), r2 0 (the appended tree is used).
	.arm
	.text
_start:
	ldr	r4, =0x8000d800		@ the watchdog
	mov	r5, #0xff
	str	r5, [r4, #4]		@ period 1.6 s
	mov	r5, #0x76
	str	r5, [r4, #0x0c]		@ feed
	ldr	r5, [r4, #0x14]		@ and clear its request
	ldr	r4, =0x03f01000
	ldr	r5, =0x4d494853		@ "SHIM"
	str	r5, [r4]
	mov	r0, #0
	mvn	r1, #0
	mov	r2, #0
	b	zimage
	.ltorg
	.align	6
zimage:
