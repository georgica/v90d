/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DVF99 clock and reset unit: the little the v90d kernel needs of it.
 * Gates and resets are numbered 0..63 as in refs/dvf99-core-facts.md 4.4.
 */
#ifndef _DVF99_CMU_H
#define _DVF99_CMU_H
#include <linux/types.h>

/* gate numbers */
#define DVF99_GATE_UART1	6
#define DVF99_GATE_UART2	7
#define DVF99_GATE_TDM1		9
#define DVF99_GATE_TIMER1	32
#define DVF99_GATE_TIMER2	33
#define DVF99_GATE_SPI2		39

/* register offsets for dvf99_cmu_read() and dvf99_cmu_update() */
#define DVF99_CMU_PLL3		0x08
#define DVF99_CMU_CPUCLK	0x14
#define DVF99_CMU_CLKSW		0x1c
#define DVF99_CMU_CLKDIV3	0x28
#define DVF99_CMU_CLKDIV4	0x2c
#define DVF99_CMU_RSTSR		0x48

int  dvf99_cmu_gate(unsigned int nr, bool on);		/* clock running or not */
int  dvf99_cmu_reset(unsigned int nr, bool assert);	/* block held in reset or not */
u32  dvf99_cmu_read(unsigned int reg);
void dvf99_cmu_update(unsigned int reg, u32 mask, u32 val);
void dvf99_restart(void);

#endif
