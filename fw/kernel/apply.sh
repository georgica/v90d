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
# the Obihai OBi200 (Marvell FFxAV)
# Its ARM926EJ-S says 0x4115926x (architecture field "v5TE", variant 1),
# which the ARM926 entry (0x4106926x, mask 0xff0ffff0) turns away: match on
# implementer and part number (value and mask both without the
# architecture and variant fields). No other processor has an
# ARM926 part number.
sed -i '/@ ARM926EJ-S (v5TEJ)/{s/0x41069260/0x41009260/;n;s/0xff0ffff0/0xff00fff0/}' "$tree/arch/arm/mm/proc-arm926.S"
sed -i '/@ ARM926EJ-S (v5TEJ)/{s/0x41069260/0x41009260/;n;s/0xff0ffff0/0xff00fff0/}' "$tree/arch/arm/boot/compressed/head.S"
add "$tree/arch/arm/Kconfig" 'source "arch/arm/mach-ffxav/Kconfig"' '^source "arch/arm/mach-ep93xx/Kconfig"'
add "$tree/arch/arm/Makefile" 'machine-$(CONFIG_ARCH_FFXAV)		+= ffxav' '^machine-.*ARCH_EP93XX'
add "$tree/arch/arm/boot/dts/Makefile" 'subdir-y += obihai' '^subdir-y .= gemini'
add "$tree/arch/arm/Kconfig.debug" '	config DEBUG_FFXAV_RAM
		bool "Kernel low-level debugging into memory on the OBi200"
		depends on ARCH_FFXAV
		help
		  Say Y here for kernel low-level debugging output into a page
		  of memory (0x03f02000) that survives a reset by the watchdog.
' '^	config DEBUG_ICEDCC'
add "$tree/arch/arm/Kconfig.debug" '	default "debug/ffxav-ram.S" if DEBUG_FFXAV_RAM' '^	default "debug/sa1100.S" if DEBUG_SA1100'
add "$tree/drivers/irqchip/Kconfig" 'config FFXAV_INTC
	bool
	select IRQ_DOMAIN
' '^endmenu'
add "$tree/drivers/irqchip/Makefile" 'obj-$(CONFIG_FFXAV_INTC)		+= irq-ffxav.o'
add "$tree/drivers/clocksource/Kconfig" 'config FFXAV_TIMER
	bool
	select TIMER_OF
' '^endmenu'
add "$tree/drivers/clocksource/Makefile" 'obj-$(CONFIG_FFXAV_TIMER)	+= timer-ffxav.o'
add "$tree/drivers/tty/serial/Kconfig" 'config SERIAL_EARLYCON_RAM
	bool "Early console into memory (earlycon=ramcon,ADDR)"
	select SERIAL_CORE
	select SERIAL_CORE_CONSOLE
	select SERIAL_EARLYCON
' '^endmenu'
add "$tree/drivers/tty/serial/Makefile" 'obj-$(CONFIG_SERIAL_EARLYCON_RAM) += earlycon-ram.o'
add "$tree/drivers/net/ethernet/marvell/Kconfig" 'config FFXAV_ETH
	tristate "Marvell FFxAV (88E7251) Ethernet and switch"
	depends on ARCH_FFXAV || COMPILE_TEST
	depends on OF && HAS_DMA
	help
	  The Ethernet controller of the Marvell 88E7251 (FFxAV) as on the
	  Obihai OBi200, and the setup of the switch it is wired to.
' '^endif # NET_VENDOR_MARVELL'
add "$tree/drivers/net/ethernet/marvell/Makefile" 'obj-$(CONFIG_FFXAV_ETH) += ffxav_eth.o'
# OBi200 telephony (TDM-NOTES.md): the GPIO pins (SLIC reset), and the
# FXS/PCM devices of the maker's telephony module on our kernel
add "$tree/drivers/gpio/Kconfig" 'config GPIO_FFXAV
	bool "Marvell FFxAV (88E7251) GPIO"
	depends on ARCH_FFXAV || COMPILE_TEST
	default y if ARCH_FFXAV
' '^endif'
add "$tree/drivers/gpio/Makefile" 'obj-$(CONFIG_GPIO_FFXAV)		+= gpio-ffxav.o'
add "$tree/drivers/misc/Kconfig" 'config FFXAV_TEL
	tristate "OBi200 telephony: /dev/fxs0, /dev/fxs1, /dev/pcm"
	depends on (ARCH_FFXAV || COMPILE_TEST) && OF && SPI && HAS_DMA
	help
	  The TDM highway of the Marvell 88E7251 (FFxAV) audio block and the
	  Si3226x SLIC on its SPI bus, with the character devices and ioctls
	  of the telephony module of the Obihai OBi200 firmware (ffxav_mp.ko),
	  for the line helper of v90d (fw/obi/obiline.c).
' '^endmenu'
add "$tree/drivers/misc/Makefile" 'obj-$(CONFIG_FFXAV_TEL)		+= ffxav_tel.o'
# OBi200 SPI controller (the flash and the SLIC are on it), watchdog and
# the LEDs on the switch PHYs (SPI-LED-WDT-NOTES.md)
add "$tree/drivers/spi/Kconfig" 'config SPI_FFXAV
	tristate "Marvell FFxAV (88E7251) SPI controller"
	depends on ARCH_FFXAV || COMPILE_TEST
	help
	  The SPI controller of the Marvell 88E7251 (FFxAV) as on the
	  Obihai OBi200: its serial flash and its SLIC are on it.
' '^comment "SPI Multiplexer support"'
add "$tree/drivers/spi/Makefile" 'obj-$(CONFIG_SPI_FFXAV)		+= spi-ffxav.o'
add "$tree/drivers/watchdog/Kconfig" 'config FFXAV_WATCHDOG
	bool "Marvell FFxAV (88E7251) watchdog"
	depends on ARCH_FFXAV
	select WATCHDOG_CORE
	help
	  The watchdog of the Obihai OBi200. It takes the feeding of the
	  watchdog over from the tick (ffxav_feed_s).
' '^endif # WATCHDOG$'
add "$tree/drivers/watchdog/Makefile" 'obj-$(CONFIG_FFXAV_WATCHDOG) += ffxav_wdt.o'
add "$tree/drivers/leds/Kconfig" 'config LEDS_FFXAV_PHY
	tristate "LEDs on the switch PHYs of the Marvell FFxAV (OBi200)"
	depends on LEDS_CLASS
	depends on ARCH_FFXAV || COMPILE_TEST
	help
	  The phone LED and part of the power LED of the Obihai OBi200,
	  forced on and off through the LED override register of the PHYs
	  of its embedded switch.
' '^comment "Flash and Torch LED drivers"'
add "$tree/drivers/leds/Makefile" 'obj-$(CONFIG_LEDS_FFXAV_PHY)		+= leds-ffxav-phy.o'
echo "v90d board support applied to $tree"
