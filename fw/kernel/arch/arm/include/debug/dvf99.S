/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Low-level debug output on a DSPG DVF99 serial port (the one the boot
 * loader left running). Written for the v90d project.
 */
#ifdef CONFIG_DEBUG_DVF99_UART2
#define DVF99_UART_PADDR	0x05b00000
#else
#define DVF99_UART_PADDR	0x05a00000
#endif
#define DVF99_UART_VADDR	0xfea00000

#define DVF99_TX_DATA		0x2c
#define DVF99_STAT		0x34
#define DVF99_STAT_TX_EMPTY	(1 << 0)
#define DVF99_STAT_TX_FULL	(1 << 1)

	.macro	addruart, rp, rv, tmp
	ldr	\rp, =DVF99_UART_PADDR
	ldr	\rv, =DVF99_UART_VADDR
	.endm

	.macro	waituartcts, rd, rx
	.endm

	.macro	waituarttxrdy, rd, rx
1001:	ldr	\rd, [\rx, #DVF99_STAT]
	tst	\rd, #DVF99_STAT_TX_FULL
	bne	1001b
	.endm

	.macro	senduart, rd, rx
	str	\rd, [\rx, #DVF99_TX_DATA]
	.endm

	.macro	busyuart, rd, rx
1002:	ldr	\rd, [\rx, #DVF99_STAT]
	tst	\rd, #DVF99_STAT_TX_EMPTY
	beq	1002b
	.endm
