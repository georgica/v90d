@ runs in kernel mode (from the patched led_ioctl), never returns:
@ interrupts off, caches flushed, identity map, caches off, MMU off, jump.
	.arm
	.text
stub:
	mrs	ip, cpsr
	orr	ip, ip, #0xc0
	msr	cpsr_c, ip
	ldr	r3, l_flush
	mov	lr, pc
	mov	pc, r3
	mov	r0, #0
	ldr	r3, l_setupmm
	mov	lr, pc
	mov	pc, r3
	ldr	r3, l_procfin
	mov	lr, pc
	mov	pc, r3
	ldr	r0, l_target
	ldr	r1, l_mach
	ldr	r2, l_atags
	ldr	r3, l_reset
	mov	pc, r3
l_flush:	.word 0xc0032030	@ arm926_flush_kern_cache_all
l_setupmm:	.word 0xc0030cc4	@ setup_mm_for_reboot
l_procfin:	.word 0xc0031c04	@ cpu_arm926_proc_fin
l_reset:	.word 0xc0031c40	@ cpu_arm926_reset
l_target:	.word 0x03800000	@ where the payload is
l_mach:		.word 0		@ r1 for the payload
l_atags:	.word 0		@ r2 for the payload
