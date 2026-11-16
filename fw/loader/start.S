/* v90d loader: where the stock boot loader's `bootm` jumps to (boot.h).
 *
 * How we are entered (HARDWARE.md, "How gsboot starts what is in core"):
 * r2 = the device tree the boot loader prepared; interrupts never enabled;
 * instruction cache on; and, through `gsboot`, MMU and data cache ON with
 * every address mapped onto itself. The flash window is mapped cached in
 * that table, and a page program is a write into that window, so the first
 * thing is to clean the data cache and switch it and the MMU off. That is
 * also the state our kernel has been started in so far.
 *
 * The program is linked at LD_LINK, where the U-Boot image header tells
 * `bootm` to put it. Started anywhere else (from RAM during bring-up) it
 * moves itself there first and tells the C part so, which then trusts
 * nothing it was handed in r2.
 *
 * ARM926EJ-S (ARMv5TEJ), ARM state. Ours; no vendor code. */
	.arch	armv5te
	.arm
	.section .text.start, "ax"
	.globl	_start
_start:
	mrs	r3, cpsr
	bic	r3, r3, #0x1f
	orr	r3, r3, #0xd3			@ supervisor mode, IRQ and FIQ masked
	msr	cpsr_c, r3
	mov	r8, r2				@ the boot loader's device tree

1:	mrc	p15, 0, r15, c7, c14, 3		@ data cache: test, clean and invalidate
	bne	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c10, 4		@ let the write buffer drain
	mcr	p15, 0, r0, c7, c5, 0		@ instruction cache: empty it, then it may be on
	mrc	p15, 0, r0, c1, c0, 0
	bic	r0, r0, #0x5			@ MMU off, data cache off; the map was one to one
	orr	r0, r0, #0x1000			@ instruction cache on (as the boot loader has it)
	mcr	p15, 0, r0, c1, c0, 0
	nop
	nop
	mov	r0, #0
	mcr	p15, 0, r0, c8, c7, 0		@ forget the translations

	adr	r0, _start			@ where we are
	ldr	r1, =_start			@ where we were linked
	mov	r9, #0
	cmp	r0, r1
	beq	3f
	mov	r9, #1				@ moved: not started by the boot loader's bootm
	ldr	r2, =__image_end
2:	ldr	r3, [r0], #4
	str	r3, [r1], #4
	cmp	r1, r2
	blo	2b
3:	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ instruction cache: nothing stale
	ldr	pc, =4f
4:	ldr	sp, =__stack_top
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
5:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	5b
	mov	r0, r8
	mov	r1, r9
	bl	ld_main				@ does not return
6:	b	6b

/* void ld_spin(uint32_t turns): a wait that touches no memory; three
 * cycles a turn with the instruction cache on */
	.globl	ld_spin
ld_spin:
	subs	r0, r0, #1
	bne	ld_spin
	mov	pc, lr

/* void ld_jump(uint32_t entry): start a Linux zImage that carries its own
 * device tree. r0 = 0, r1 = 0 (no machine number: the kernel is device tree
 * only), r2 = 0 (no tags and no tree from us: the appended one is used). */
	.globl	ld_jump
ld_jump:
	mov	r4, r0
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0		@ the image was just written: instruction cache
	mov	r1, #0
	mov	r2, #0
	mov	pc, r4
	.ltorg
	.section .note.GNU-stack,"",%progbits
