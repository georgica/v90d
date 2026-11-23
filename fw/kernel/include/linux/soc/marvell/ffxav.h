/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Marvell FFxAV (88E7251), as on the Obihai OBi200: what its drivers share.
 *
 * Written for the v90d project.
 */
#ifndef __LINUX_SOC_MARVELL_FFXAV_H
#define __LINUX_SOC_MARVELL_FFXAV_H

/*
 * The tick (drivers/clocksource/timer-ffxav.c) feeds the watchdog until
 * the watchdog driver calls this; from then on only the watchdog driver
 * (through the watchdog core) does.
 */
void ffxav_timer_stop_feeding(void);

#endif
