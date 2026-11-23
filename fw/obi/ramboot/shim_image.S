@ The boot shim in front of an uncompressed kernel Image RAM-booted by kjump
@ on the OBi200 (kjump unpacks the payload, .xz, while the stock kernel
@ still feeds the watchdog; there is no decompressor to outlast it).
@ It runs at 0x03800000, MMU and caches off, interrupts off. Layout, by
@ mkpayload.py: this shim (0x100 octets; the Image's and the device tree's
@ lengths in its last two words), the Image (padded to 4), the device tree.
@ It feeds the watchdog (longest period, 1.6 s), leaves its mark
@ (0x03f01000 "SHIM"), copies the Image to 0x8000 and the tree to
@ 0x02000000, and enters the Image: r0 0, r1 ~0 (a device tree), r2 the tree.
	.arm
	.text
_start:
	ldr	r4, =0x8000d800		@ the watchdog
	mov	r5, #0xff
	str	r5, [r4, #4]		@ period 1.6 s
	mov	r5, #0x76
	str	r5, [r4, #0x0c]		@ feed
	ldr	r5, [r4, #0x14]		@ and clear its request
	ldr	r5, =0x03f01000
	ldr	r6, =0x4d494853		@ "SHIM"
	str	r6, [r5]
	adr	r6, _start
	ldr	r7, [r6, #0xf8]		@ the Image's length (a multiple of 4)
	ldr	r8, [r6, #0xfc]		@ the tree's length
	add	r9, r6, #0x100		@ the Image
	ldr	r10, =0x8000
	add	r11, r9, r7
1:	cmp	r9, r11			@ the Image to 0x8000
	ldrlo	r0, [r9], #4
	strlo	r0, [r10], #4
	blo	1b
	mov	r5, #0x76
	str	r5, [r4, #0x0c]		@ feed again
	ldr	r10, =0x02000000
	add	r11, r9, r8
2:	cmp	r9, r11			@ the tree to 0x02000000
	ldrlob	r0, [r9], #1
	strlob	r0, [r10], #1
	blo	2b
	mov	r0, #0
	mvn	r1, #0
	ldr	r2, =0x02000000
	ldr	pc, =0x8000
	.ltorg
	.org	0xf8
	.word	0			@ the Image's length
	.word	0			@ the tree's length
