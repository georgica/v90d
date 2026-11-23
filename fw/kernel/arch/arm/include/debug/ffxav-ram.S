/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Low-level debug output into memory, for the Obihai OBi200, whose serial
 * port nobody can reach without opening the box. 64 KiB at 0x03f20000
 * (outside the kernel's memory, kept across the watchdog's reset) get the
 * number of octets at +4 and the text from +8; it stops when they are full.
 * The board code (mach-ffxav) keeps the megabyte around it mapped, so with
 * earlyprintk this carries the kernel's whole log.
 * The decompressor runs with the data cache on, so each octet's cache line
 * is cleaned to memory at once (a reset would lose it otherwise); the
 * kernel maps the page uncached.
 *
 * senduart uses ip: every caller (debug.S, the decompressor's putc) is a
 * function, and ip is a scratch register across calls.
 *
 * Written for the v90d project.
 */
#define FFXAV_DLL_PADDR		0x03f20000
#define FFXAV_DLL_VADDR		0xfe820000
#define FFXAV_DLL_ROOM		0xff00

	.macro	addruart, rp, rv, tmp
	ldr	\rp, =FFXAV_DLL_PADDR
	ldr	\rv, =FFXAV_DLL_VADDR
	.endm

	.macro	waituartcts, rd, rx
	.endm

	.macro	waituarttxrdy, rd, rx
	.endm

	.macro	senduart, rd, rx
	ldr	ip, [\rx, #4]
	cmp	ip, #FFXAV_DLL_ROOM
	addlo	ip, ip, \rx
	strblo	\rd, [ip, #8]
	addlo	ip, ip, #8
	mcrlo	p15, 0, ip, c7, c10, 1		@ clean that line of the data cache
	sublo	ip, ip, \rx
	sublo	ip, ip, #7			@ the next free octet
	strlo	ip, [\rx, #4]
	mcrlo	p15, 0, ip, c7, c10, 1		@ (cleans the line at that number: harmless)
	.endm

	.macro	busyuart, rd, rx
	.endm
