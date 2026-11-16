#!/bin/sh
# Put the v90d board support into a pristine Linux 6.6 tree.
#   fw/kernel/apply.sh <linux-tree>
# Copies our files in and adds the few lines of Kconfig and Makefile glue.
# Safe to run again.
set -e
here=$(cd "$(dirname "$0")" && pwd)
tree=$1
[ -f "$tree/Makefile" ] || { echo "usage: $0 <linux-tree>" >&2; exit 2; }

(cd "$here" && find arch drivers include -type f) | while read -r f; do
    mkdir -p "$tree/$(dirname "$f")"
    cp "$here/$f" "$tree/$f"
done

# the line driver's portable part lives in fw/drv, where its tests are
for f in pump proslic slic; do
    cp "$here/../drv/$f.c" "$here/../drv/$f.h" "$tree/drivers/misc/v90line/"
done
cp "$here/../drv/qspi.c" "$tree/drivers/mtd/devices/dvf99-qspi-core.c"
cp "$here/../drv/qspi.h" "$tree/drivers/mtd/devices/qspi.h"

# add LINE to FILE unless it is there; with a third argument, before the
# first line matching that pattern, else at the end
add() {
    first=$(printf '%s\n' "$2" | head -n 1)
    grep -qxF "$first" "$1" && return 0
    if [ -n "$3" ]; then
        printf '%s\n' "$2" > "$1.ins"
        awk -v ins="$1.ins" -v p="$3" '!d && $0 ~ p { while ((getline l < ins) > 0) print l; d = 1 } { print }' "$1" > "$1.new" && mv "$1.new" "$1"
        rm -f "$1.ins"
    else
        printf '%s\n' "$2" >> "$1"
    fi
}

add "$tree/arch/arm/Kconfig" 'source "arch/arm/mach-dvf99/Kconfig"' '^source "arch/arm/mach-ep93xx/Kconfig"'
add "$tree/arch/arm/Makefile" 'machine-$(CONFIG_ARCH_DVF99)		+= dvf99' '^machine-.*ARCH_EP93XX'
add "$tree/arch/arm/boot/dts/Makefile" 'subdir-y += dspg' '^subdir-y .= gemini'

add "$tree/arch/arm/Kconfig.debug" '	config DEBUG_DVF99_UART
		bool "Kernel low-level debugging on a DSPG DVF99 serial port"
		depends on ARCH_DVF99
		help
		  Say Y here for kernel low-level debugging output on the
		  serial port the boot loader of a DVF99 board left running.
' '^	config DEBUG_ICEDCC'
add "$tree/arch/arm/Kconfig.debug" '	default "debug/dvf99.S" if DEBUG_DVF99_UART' '^	default "debug/sa1100.S" if DEBUG_SA1100'
add "$tree/arch/arm/Kconfig.debug" 'config DEBUG_DVF99_UART2
	bool "Low-level debugging on the second DVF99 serial port (0x05b00000)"
	depends on DEBUG_DVF99_UART
'

add "$tree/drivers/irqchip/Kconfig" 'config DVF99_PLICU
	bool
	select IRQ_DOMAIN
' '^endmenu'
add "$tree/drivers/irqchip/Makefile" 'obj-$(CONFIG_DVF99_PLICU)		+= irq-dvf99-plicu.o'

add "$tree/drivers/clocksource/Kconfig" 'config DVF99_TIMER
	bool
	select CLKSRC_MMIO
	select TIMER_OF
' '^endmenu'
add "$tree/drivers/clocksource/Makefile" 'obj-$(CONFIG_DVF99_TIMER)	+= timer-dvf99.o'

add "$tree/drivers/tty/serial/Kconfig" 'config SERIAL_DVF99
	bool "DSPG DVF99 serial port"
	depends on ARCH_DVF99
	select SERIAL_CORE

config SERIAL_DVF99_CONSOLE
	bool "Console on a DSPG DVF99 serial port"
	depends on SERIAL_DVF99
	select SERIAL_CORE_CONSOLE
	select SERIAL_EARLYCON
' '^endmenu'
add "$tree/drivers/tty/serial/Makefile" 'obj-$(CONFIG_SERIAL_DVF99) += dvf99_uart.o'

add "$tree/drivers/gpio/Kconfig" 'config GPIO_DVF99
	bool "DSPG DVF99 GPIO"
	depends on ARCH_DVF99
	default y
' '^endif'
add "$tree/drivers/gpio/Makefile" 'obj-$(CONFIG_GPIO_DVF99)		+= gpio-dvf99.o'
add "$tree/drivers/misc/Kconfig" 'source "drivers/misc/v90line/Kconfig"' '^endmenu'
add "$tree/drivers/misc/Makefile" 'obj-$(CONFIG_V90LINE)		+= v90line/'
add "$tree/drivers/watchdog/Kconfig" 'config DVF99_WATCHDOG
	bool "DSPG DVF99 watchdog"
	depends on ARCH_DVF99
	select WATCHDOG_CORE
' '^endif # WATCHDOG$'
add "$tree/drivers/watchdog/Makefile" 'obj-$(CONFIG_DVF99_WATCHDOG) += dvf99_wdt.o'
add "$tree/drivers/mtd/devices/Kconfig" 'config MTD_DVF99_QSPI
	bool "HT802 serial flash on the DSPG DVF99 QSPI controller"
	depends on ARCH_DVF99
' '^endmenu'
add "$tree/drivers/mtd/devices/Makefile" 'obj-$(CONFIG_MTD_DVF99_QSPI)	+= dvf99-qspi.o dvf99-qspi-core.o'
echo "v90d board support applied to $tree"
