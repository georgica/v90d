# DVF9918 core hardware facts (for from-scratch mainline board support)

Scope: hardware facts only (registers, bits, sequences, numbers), restated in
my own words from reading the vendor GPL trees. No vendor code is reproduced.
This complements `HARDWARE.md` and `refs/dvf99-kernel-research.md`; it does not
repeat them.

Sources and short names used in citations:

- **K** = vendor kernel tree `linux-3.4.20` (paths relative to its root)
- **U** = vendor U-Boot tree `uboot` (paths relative to its root)
- **dts** = `~/v90d/recon/backup/board.dts` (this unit's DTB, decompiled)
- **backup** = `~/v90d/recon/backup/*.bin` (this unit's flash)
- **strings** = `~/v90d/recon/out/uboot.strings`, `vmlinux.strings` (shipped binaries)
- **log-v1** = `refs/ht802/tools/FirmwareDumps/HT802-1.0.0.24/bootlog.md`
  (an older V1.x HT802, same SoC, same boot chain)

Confidence tags: **[src]** read directly in the GPL source; **[unit]** checked
against this unit's own flash/DTB; **[log]** from the V1.x boot log;
**[infer]** my inference, not stated anywhere.

Caveat that applies everywhere: the GPL trees are from 2015 to 2017. The
bootloader and kernel actually shipped on this V2.5A unit are newer builds and
differ in places (shown where I could see it in the strings).

---

## 0. Quick reference

| Item | Value | Tag |
|---|---|---|
| PLL1 / CPU | 400 MHz | [log] |
| AHB/APB peripheral clock ("hclk", "PCLK") | 400/3 = 133.33 MHz | [log]+[src] |
| Timer tick input | hclk/16 = 8 333 333 Hz | [src] |
| RTC counter ("slow clock") | 1 MHz | [src]+[log] |
| Main oscillator | 25 or 50 MHz, strap selected (CMU RSTSR bit 2) | [src] |
| RAM | 32 MiB SDRAM at 0xC8000000 (MPMC, not the Denali DDR controller) | [log] |
| Kernel load = entry | 0xC8008000, legacy uImage, comp "none", wrapping a zImage | [unit] |
| uImage staging address | 0xC8800000 | [src] |
| DTB staging address | 0xC8C00000, then relocated by U-Boot to about 0xC9D2C000 | [src]+[log] |
| Vendor PAGE_OFFSET | 0xD0000000 | [src] |

---

## 1. Interrupt controller ("PLICU", 0x05100000)

Source: K `arch/arm/common/plicu.c`, K `arch/arm/include/asm/hardware/plicu.h`,
K `arch/arm/mach-dvf/include/mach/irqs.h`.

### 1.1 Shape

- 64 sources, two 32-bit register halves. Half "1" covers sources 0..31, half
  "2" covers 32..63. Bit n of a half is source n (or 32+n).
- Separate IRQ and FIQ outputs. Any source can be routed to FIQ (FIQ_SEL).
  The vendor Linux driver uses IRQ only.
- All registers are 32-bit, little endian, at 4-byte stride.

### 1.2 Register map

| Offset | Name | Use |
|---|---|---|
| 0x00 / 0x04 | CH_CLR_INT1/2 | write 1 = acknowledge (clear) the latched request of an edge source |
| 0x08 / 0x0C | CH_EDGE_LEVEL1/2 | per source trigger: 1 = rising edge, 0 = active-high level |
| 0x10 / 0x14 | CH_SW_TRIG_SET1/2 | software trigger (not used by the driver) |
| 0x18 | SW_CH_REQ | software request (not used) |
| 0x1C / 0x20 | STATUS1/2 | not used by the driver; meaning not documented in the tree |
| 0x24 / 0x28 | PENDING_REQ1/2 | not used by the driver |
| 0x2C / 0x30 | CAUSE1/2 | **active sources for the IRQ output** (what the handler reads) |
| 0x34 / 0x38 | CH_MASK1/2 | read back the enable mask (1 = enabled) |
| 0x3C / 0x40 | SET_CH_MASK1/2 | write 1 = **enable (unmask)** a source |
| 0x44 / 0x48 | CLR_CH_MASK1/2 | write 1 = **disable (mask)** a source |
| 0x4C .. 0x68 | CH_PRIORITY1..8 | 8 words = 4 bits per source (64 x 4 = 256 bits); never written by Linux or U-Boot |
| 0x6C / 0x70 | FIQ_SEL1/2 | route source to FIQ |
| 0x74 / 0x78 | FIQ_PENDING_REQ1/2 | |
| 0x7C / 0x80 | FIQ_CAUSE1/2 | |
| 0x84 | PRI_MASK | priority threshold, IRQ (never written) |
| 0x88 | PRI_MASK_FIQ | priority threshold, FIQ (never written) |

Note the naming trap (the vendor source comments on it too): the "mask"
registers are really enable registers. SET_CH_MASK enables, CLR_CH_MASK
disables. [src: `plicu_mask`, `plicu_unmask`]

### 1.3 Operation as the vendor driver does it

- Init (`plicu_init`): write 0xFFFFFFFF to both CLR_CH_MASK (all disabled),
  then 0xFFFFFFFF to both CH_CLR_INT (clear all latches). Priorities and
  PRI_MASK are left at reset values.
- Mapping a source (`plicu_irq_map`): clear its EDGE_LEVEL bit, so the default
  is level, active high.
- Trigger types (`plicu_set_type`): only **rising edge** (bit = 1) and
  **level high** (bit = 0) exist. Falling edge and level low are rejected.
  Read-modify-write of EDGE_LEVEL with interrupts off.
- Ack (`plicu_ack`): write the source bit to CH_CLR_INT. Needed for edge
  sources only.
- Dispatch (`plicu_handle_irq`): read CAUSE1; service every set bit lowest
  first (find-first-set); read CAUSE2, same with +32; loop until both read 0.
  There is no vector/"highest priority source" register in use and no
  end-of-interrupt write.
- "Is anything pending" (`plicu_pending`, used by idle): CAUSE1 | CAUSE2 != 0.
- Suspend: save CH_MASK1/2, disable everything not marked wake-capable; resume
  re-enables through SET_CH_MASK.

### 1.4 Source numbers (hwirq)

| # | Source | # | Source | # | Source |
|---|---|---|---|---|---|
| 0 | MCU ARM926 | 22 | NAND/flash controller | 44 | SPI2 |
| 1 | CCU1 error | 23 | **Ethernet MAC (GMAC)** | 45 | SPI1 |
| 2 | USB2 OTG SOF | 24..30 | **EXTINT 0..6** | 46 | timer 4 |
| 3 | USB2 OTG | 31 | SW2 | 47 | SW3 |
| 4 | keypad | 32..40 | **EXTINT 7..15** | 48 | timer 3 |
| 5 | CCU0 error | 41 | RTC | 49 | timer 2 |
| 6 | CSS (DSP core) | 42 | I2C2 | 50 | **timer 1** |
| 7 | security accelerator | 43 | I2C1 | 53 | GPU |
| 8 | DRAM controller | | | 54 | **UART2 (0x05B00000)** |
| 9..11 | PMU (a2d comp, bat ok, analog) | | | 55 | UART1 (0x05A00000) |
| 12 | CIU (camera) | | | 59 | TDM2 |
| 13 | slave MII | | | 60 | TDM1 |
| 14 | LCD controller | | | 62 | watchdog |
| 15 | SW1 | | | 63 | SW4 |
| 16 | **QSPI** | | | | |
| 17 | SD/MMC | | | | |
| 18 | USB 1.1 | | | | |
| 19 | SEC (inter-CPU mailbox) | | | | |
| 20 | DMA error | | | | |
| 21 | DMA done | | | | |

51, 52, 56, 57, 58, 61 are reserved. [src: `irqs.h`]

### 1.5 Numbering in the device tree vs Linux numbers

- `#interrupt-cells = <1>`; the single cell is the **raw PLICU source number**
  (0..63). No offset, no flags cell. [dts], [src: `irq_domain_xlate_onecell`]
  Examples from the dts: timer 0x32 = 50, UART2 0x36 = 54, GMAC 0x17 = 23,
  QSPI 0x10 = 16, SPI2 0x2c = 44, TDM1 0x3c = 60, RTC 0x29 = 41.
- The vendor kernel uses a linear irq domain of 64 with SPARSE_IRQ, 16 legacy
  numbers reserved. Linux irq numbers are therefore handed out **dynamically in
  mapping order** starting at 16; there is no fixed "hwirq + N" relation.
  That is why `/proc/interrupts` on this unit shows GMAC = 40 (hwirq 23),
  SPI2 = 41 (hwirq 44), QSPI = 42 (hwirq 16), TDM1 = 43 (hwirq 60), and the
  16 GPIO upstream lines = 18..33.

### 1.6 GPIO interrupts (cascade)

Source: K `drivers/gpio/gpio-dspg.c`, K `mach/platform.h`, K `devtree/dvf99.dtsi`.

- There is no per-pin GPIO interrupt. The SoC has **16 "external interrupt"
  (EXTINT) lines**, each hard-wired to one specific pad, and each with **its
  own PLICU source**. It is a 1:1 cascade, not a shared demultiplexed line.
- EXTINT n uses PLICU source: n = 0..6 -> 24..30; n = 7..15 -> 32..40.
- EXTINT to pad map (dts property `dspg,extints`, pairs of bank, pin):

  | EXTINT | Pad | EXTINT | Pad | EXTINT | Pad | EXTINT | Pad |
  |---|---|---|---|---|---|---|---|
  | 0 | AGPIO16 | 4 | BGPIO26 | 8 | DGPIO26 | 12 | EGPIO6 |
  | 1 | AGPIO17 | 5 | CGPIO28 | 9 | DGPIO24 | 13 | DGPIO30 (or CGPIO31, see below) |
  | 2 | AGPIO18 | 6 | DGPIO8 | 10 | DGPIO25 | 14 | DGPIO31 |
  | 3 | AGPIO19 | 7 | DGPIO9 | 11 | BGPIO31 | 15 | BGPIO11 |

  EXTINT13 and EXTINT14 have an alternate pad location selected by SYSCFG
  SPCR2 bits 17 and 16. The vendor driver sets bit 17 (EXT13 -> CGPIO31) when
  CGPIO31 is requested. [src: `dspg_gpiolib_request`]
- EXTINT control registers live at the end of the GPIO block (0x05000000):

  | Offset | Name | Use |
  |---|---|---|
  | 0x2A0 | EXTINT_CFG | 2 bits per EXTINT at bit 2n: bit 0 = polarity (1 = high / rising, 0 = low / falling), bit 1 = mode (1 = edge, 0 = level) |
  | 0x2A4 | EXTINT_EDGE | defined, not used by the driver |
  | 0x2A8 | EXTINT_STAT | defined, not used by the driver |
  | 0x2AC | EXTINT_EDGE_CLEAR | write bit n = clear the edge latch of EXTINT n |

- The vendor driver has no mask bit in the EXTINT block: masking an EXTINT is
  done by disabling its upstream PLICU source. Ack writes EDGE_CLEAR. "Both
  edges" is emulated: configure level with polarity opposite to the current pin
  level, and flip the polarity in ack/unmask.
- The upstream PLICU sources are left at the default (level high) by the
  vendor driver; the EXTINT block does the edge detection.
- GPIO interrupt specifier in the vendor binding: 2 cells, (EXTINT number,
  flags), domain of 16. `gpio_to_irq` works only for the 16 pads above.
- On this unit: SLIC interrupts are AGPIO16 and AGPIO17 = EXTINT0 and EXTINT1
  = PLICU sources 24 and 25. [dts `silabs_int`]

---

## 2. System timer, clocksource, sched_clock

Source: K `drivers/clocksource/clksrc-dspg-timer.c`, `clksrc-dspg-rtc.c`,
K `arch/arm/mach-dvf/time.c`, U `arch/arm/cpu/arm926ejs/dvf99/timer.c`.

### 2.1 Which block does what in the vendor kernel

| Role | Block | Base | PLICU source | Rate |
|---|---|---|---|---|
| clockevent (periodic + oneshot, tick) | "timer1" | 0x05800000 | 50 | hclk/16 = 8.333 MHz |
| clocksource `dspg-rtc`, rating 100, 32 bit | RTC | 0x05600000 | (41, unused for this) | slow clock = 1 MHz |
| sched_clock, 32 bit | same RTC counter | | | 1 MHz (log: "sched_clock: 32 bits at 1000kHz") |
| persistent clock | same RTC counter (delta accumulation) | | | |

HZ = 100, NO_HZ and high-res timers enabled in the vendor config.

### 2.2 Timer block registers (4 timers)

Timers sit in pairs: 0x05800000 (timer1), 0x05800020 (timer2), 0x05900000
(timer3), 0x05900020 (timer4). PLICU sources 50, 49, 48, 46.

| Offset | Name | Notes |
|---|---|---|
| 0x00 | LOAD | reload / start value |
| 0x04 | VALUE | current value, **counts down** |
| 0x08 | CONTROL | see bits |
| 0x0C | INTCLR | any write (vendor writes 0) clears the interrupt |

CONTROL bits: bit 0 one-shot, bit 1 32-bit counter (else 16), bits 3:2
prescaler (0 = /1, 1 = /16, 2 = /256), bit 5 interrupt enable, bit 6 periodic
mode, bit 7 run.

[infer] This layout and these bit positions are exactly the ARM SP804
dual-timer layout (Load, Value, Control, IntClr, second timer at +0x20). The
vendor code never touches 0x10/0x14/0x18 (RIS, MIS, BGLoad) and never reads
PrimeCell ID registers, so whether those exist is unverified. A mainline
`arm,sp804` node (which does not check the PrimeCell ID on the OF path) is a
plausible way to get both clockevent and clocksource without writing a driver.

How the vendor kernel drives timer1:

- Input clock = AHB clock ("timer1" gate), prescaler /16 always. With hclk =
  133 333 333 Hz the count rate is 8 333 333 Hz.
- Periodic: CONTROL = 0, LOAD = ceil(rate/HZ) (83 334 at HZ=100), CONTROL =
  32bit | /16 | IE | RUN | PERIODIC (0xE6).
- One-shot `set_next_event(delta)`: LOAD = delta, then CONTROL = 32bit | /16 |
  IE | RUN | ONESHOT (0xA7), with interrupts off. Min delta 3, max 0xFFFFFFF0.
- Interrupt: write INTCLR, call the event handler.
- Shutdown gates the timer clock off (CMU), resume gates it on.

State at kernel entry: U-Boot uses the same timer1 as a free-running 32-bit
down counter, /16, LOAD = 0xFFFFFFFF, run bit set, interrupt disabled
(CONTROL = 0x86), and never stops it. [src: U `timer_init`]

### 2.3 RTC block registers (0x05600000)

| Offset | Name | Notes |
|---|---|---|
| 0x00 | CFG | vendor writes 0 at init |
| 0x04 | CTL | |
| 0x08 | ALARMVAL | |
| 0x0C | STATUS | |
| 0x10 | INTCLR | |
| 0x14 | TIME | free-running 32-bit **up** counter at the slow clock rate |

- The counter is clocked asynchronously to the bus. The vendor read routine
  reads TIME repeatedly until two consecutive reads agree. [src: `dspg_rtc_read`]
- Clock: "rtc" = gate bit 2 of SWCLKENR1, parent "slowclk" = main clock divided
  by (SLOWDIV+1), SLOWDIV = bits 14:4 of CMU CLKSWCNTRL, enabled by bit 3 of
  CLKSWCNTRL. U-Boot sets SLOWDIV so the result is 1 MHz (25 MHz -> 24,
  50 MHz -> 49) but only writes the divider; it does not set bit 3 or the RTC
  gate, and does not release the RTC reset (reset bit 2). The vendor kernel
  does all three. [src: U `clk_init`, K `clock.c`]
- A separate RTC class driver (K `drivers/rtc/rtc-dspg.c`) exists for the
  alarm/time function; not needed for bring-up.

### 2.4 What a mainline port can do

- Simplest: timer1 as clockevent (source 50), timer2 (0x05800020, gate bit 33,
  shares reset bit 32 with timer1, source 49) as a free-running down-counting
  clocksource and sched_clock. Both from a fixed 133.33 MHz clock. That avoids
  the RTC and its extra clock/reset setup.
- If the RTC counter is wanted: release reset bit 2, set gate bit 2, set
  CLKSWCNTRL bit 3, write CFG = 0, then use TIME as a 32-bit up counter at
  1 MHz with the double-read.

---

## 3. UART

Source: K `drivers/tty/serial/dspg-uart.c`, K `mach/uncompress.h`,
U `arch/arm/cpu/arm926ejs/dvf99/serial.c`, U `board/dspg/evb9918/serial.c`.

**It is a custom DSPG block.** Despite the "DW_UART" name in the vendor driver
it is not a Synopsys DesignWare 8250, not a 16550 and not a PL011. There is no
LCR/DLAB, no IIR, no USR/busy-detect register. No mainline driver matches;
`snps,dw-apb-uart`, `ns16550a` and `arm,pl011` cannot drive it. A small custom
serial driver (plus earlycon) is required. The vendor compatible is
`dspg,uart`, port type name "DSPG_UART", device names ttyS0/ttyS1.

### 3.1 Instances

| Port | Base | PLICU source | Clock gate / reset bit | Pads |
|---|---|---|---|---|
| UART1 (alias serial0, ttyS0) | 0x05A00000 | 55 | SWCLKENR1 bit 6 / SWCHWRST1 bit 6 | DGPIO20 TX, DGPIO21 RX |
| UART2 (alias serial1, ttyS1) | 0x05B00000 | 54 | bit 7 / bit 7 | location 0: EGPIO16 TX, EGPIO17 RX; location 1: AGPIO28 TX, AGPIO29 RX |

Region size 0x200 in the dts (driver maps 4 KiB). Input clock = AHB clock =
133.33 MHz on this board.

### 3.2 Registers

Vendor Linux accesses them as 16-bit, vendor U-Boot as 32-bit; both work, so
treat them as 16 significant bits in 32-bit slots, 4-byte stride.

| Offset | Name | Bits |
|---|---|---|
| 0x00 | CTL | bit 0 = UART enable; bit 7 = block reset |
| 0x04 | CFG | bits 3:2 data length (00 = 8, 01 = 7, 10 = 9); bit 4 two stop bits; bit 5 parity odd (0 = even); bit 6 parity enable; bit 7 software reset; bit 8 "OD" enable; bit 9 RTS flow control enable; bit 10 CTS flow control enable; bit 11 break detection enable; bits 13, 14 IrDA TX/RX invert; bit 15 IrDA mode |
| 0x08 | INT_DIV | integer baud divisor |
| 0x0C | FRAC_DIV | fractional divisor, 4 bits (sixteenths) |
| 0x10 | TX_WM | TX FIFO watermark |
| 0x14 | RX_WM | RX FIFO watermark |
| 0x18 | FIFO_ISR | interrupt status |
| 0x1C | FIFO_IER | interrupt enable |
| 0x20 | FIFO_ICR | interrupt clear (write 1) |
| 0x24 | TX_LVL | TX FIFO fill level (0x80 = full) |
| 0x28 | RX_LVL | RX FIFO fill level |
| 0x2C | TX_DATA | write a character |
| 0x30 | RX_DATA | bits 8:0 data, bit 9 parity error, bit 10 framing error |
| 0x34 | STAT | bit 0 TX FIFO empty; bit 1 TX FIFO full; bit 2 RTS pin (inverted); bit 3 CTS pin (inverted); bit 8 RX FIFO empty; bit 9 RX FIFO full; bit 10 break detected |

Interrupt bits (same positions in ISR, IER, ICR): bit 0 TX overrun, bit 1 TX
below watermark, bit 8 RX underrun, bit 9 RX overrun, bit 10 RX above
watermark, bit 11 RX timeout, bit 12 break. Only bits 0, 8, 9, 12 are clearable
through ICR; the watermark and timeout bits follow the FIFO state.

FIFO depth: 128 each way. [src: `UART_FIFO_SIZE`, and the decompressor treats
TX_LVL == 0x80 as full]

### 3.3 Baud rate

divisor16 = uartclk / baud (integer division); INT_DIV = divisor16 >> 4,
FRAC_DIV = divisor16 & 0xF. So the baud clock is uartclk / (16 x (INT +
FRAC/16)). At 133 333 333 Hz and 115200: divisor16 = 1157, INT_DIV = 72,
FRAC_DIV = 5. [src: `dw_uart_set_termios`, U `serial_init_dev`]

### 3.4 Sequences

- Init / set mode: CTL = 0 (disable), write CFG, write dividers, clear
  interrupts (ICR = 0x1301), TX_WM = RX_WM = 64, IER as needed, CTL = 1.
  The vendor kernel's `startup` deliberately does not touch the dividers and
  inherits U-Boot's until termios is set.
- Polled TX: wait while STAT bit 1 (TX full), write TX_DATA. Flush: wait for
  STAT bit 0 (TX empty) or TX_LVL == 0.
- Polled RX: char available when STAT bit 8 is clear; read RX_DATA, check bits
  9/10.
- IRQ RX: enable bits 9, 10, 11, 12; drain while STAT bit 8 is clear. IRQ TX:
  enable bits 0 and 1 while there is data, fill up to half the FIFO per pass.
- The vendor driver defers all work to a tasklet with the IRQ line disabled;
  that is a design choice, not a hardware need.

### 3.5 Console selection and quirks

- U-Boot picks the console UART from straps: field = CMU RSTSR bits 15:8;
  (field & 0x11) == 0x11 -> UART1; == 0x01 -> UART2 on EGPIO16/17; == 0x10 ->
  UART2 on AGPIO28/29. [src: U `get_uart_config`, CONFIG_USE_UART = 0 for
  evb9918_ht] Only the selected UART gets reset release, clock gate, pad mux.
- The vendor decompressor finds the active UART by checking the clock gate
  bits 6/7 at 0x05300058 and CTL bit 0, UART1 first. An earlycon can use the
  same test.
- Grandstream lock: the vendor kernel has a global "gxp_console" flag,
  default 1, cleared by the `gxp_console=0` argument (or an ATAG). While it is
  1 the driver refuses to probe the port with alias serial0 (UART1). Irrelevant
  to a new kernel except that the string `gxp_console=...` will appear in the
  bootargs. [src: K `arch/arm/kernel/setup.c`, `dspg-uart.c` probe]
- DMA channels exist for the UART FIFOs (2..5) but are unused by the driver.

---

## 4. Clock and reset controller ("CMU", 0x05300000)

Source: K `arch/arm/mach-dvf/clock.c`, K `mach/hardware.h`, K `mach/reset.h`,
U `arch/arm/cpu/arm926ejs/dvf99/clock.c`.

Naming trap: K `arch/arm/mach-dvf/ccu.c` is **not** a clock unit. "CCU" is a
bus *coherency* unit at 0x07000000 (section 9.4). Clocks and resets are all in
the CMU.

### 4.1 CMU register map

| Offset | Name | Contents |
|---|---|---|
| 0x00 | PLL1CONTROL | system PLL |
| 0x04 | PLL2CONTROL | second PLL (DRAM clock on DDR boards, optional peripheral source) |
| 0x08 | PLL3CONTROL | audio PLL |
| 0x14 | CPUCLKCNTRL | bits 3:0 CPU divider (rate = sysclk/(n+1)); bits 5:4 AHB divider; bits 15:14 alt-sysclk source; bits 17:16 audio source |
| 0x18 | COMCLKSEL | CSS (DSP core) clocks: bits 3:0 prediv, bits 8:4 ADPCM div, bit 9 BMP /12, bits 11:10 CSS source, bit 14 BMP source |
| 0x1C | CLKSWCNTRL | switches, see 4.3 |
| 0x20 | CLKDIVCNTRL1 | bits 7:0 CIU, 15:8 LCD, 19:16 ETM, bit 23 RMII clock source, 31:24 SD/MMC |
| 0x24 | CLKDIVCNTRL2 | bits 5:0 internal Ethernet divider, 7:6 refout source, 15:8 ISDN, 19:16 GPU, 27:20 Ethernet reference divider |
| 0x28 | CLKDIVCNTRL3 | bits 7:0 refout, 15:8 **QSPI**, 25:16 D-class, 31:24 **SPI2** |
| 0x2C | CLKDIVCNTRL4 | bits 11:0 **TDM1**, 23:12 TDM2, 27:24 USB1.1, 29:28 USB1.1 source |
| 0x30 | CLKDIVCNTRL5 | bits 19:12 clkout divider, 31:24 APU |
| 0x3C | CLKOSCCR | bit 0 DECT oscillator off (0 = running), bit 2 12 MHz oscillator enable, bit 5 12 MHz stable |
| 0x40 | SWSYSRST | system reset: write 0x7496398B then 0xB5C9C674 |
| 0x44 | SWWRMRST | warm reset (unused in the tree) |
| 0x48 | RSTSR | straps and reset cause, see 4.2 |
| 0x50 | SWCHWRST1 | block resets 0..31, **1 = held in reset** |
| 0x54 | SWCHWRST2 | block resets 32..63 |
| 0x58 | SWCLKENR1 | clock gates 0..31, **1 = running** |
| 0x5C | SWCLKENR2 | clock gates 32..63 |
| 0x60 | CUSTATR | status (unused in the tree) |
| 0x68 | TEST | bits 4:0 clkout source select; bit 11 CSS ARM debug clock; bit 12 ARM debug clock |

Every divider field means "divide by (field + 1)".

Access rules seen in the vendor code:
- Gate and reset registers are updated read-modify-write with interrupts and
  FIQ off, and **written again until the read-back matches**
  (`set_on_off_gated`, `reset_set_onoff`). [infer] the CSS core can write the
  same registers concurrently; the header marks COMCLKSEL, SWCHWRST1/2 and
  SWCLKENR1/2 as "guarded by SEC CMU mutex". With the CSS core held in reset
  (our design) plain read-modify-write is safe.

### 4.2 RSTSR (0x48) fields used

| Bits | Meaning |
|---|---|
| 1 | strap: main clock taken from the 13.824 MHz DECT oscillator instead of the main oscillator |
| 2 | strap: main oscillator is 25 MHz (1) or 50 MHz (0) |
| 3 | watchdog reset indication |
| 4, 5 | software reset indications. Vendor code writes 1 to bits 3, 4, 5 before a software reset ("clear the indications") |
| 7:6 | strap: source of the CLKOUT pad (0 main clock, 1 CPU, 2 AHB, 3 clkout divider) |
| 15:8 | strap field used for console UART selection (section 3.5) |

### 4.3 Clock tree and rate derivation

Roots: main oscillator (25/50 MHz by strap), DECT oscillator 13.824 MHz,
12 MHz oscillator (USB), "PLL4" fixed 480 MHz (USB PHY; U-Boot calls it 48 MHz).

PLL register format (PLL1/2/3 identical):

| Bits | Field |
|---|---|
| 4:0 | pre-divider R = field + 1 |
| 11:5 | multiplier F = field + 1 |
| 13:12 | output divider, divide by 2^field |
| 14 | VCO band, set when VCO > 600 MHz |
| 15 | power down (1 = off) |
| 16 | bypass (output = input) |
| 18 | lock (read only) |
| 20:19 | PLL1 only: input select 0 = main clock, 1 = DECT osc, 2 = 12 MHz osc |

fout = fin x F / R / 2^OD. Limits used by the vendor search: phase detector
input (fin/R) 10..50 MHz, VCO (fin x F / R) 300..1000 MHz, F between 6 and 100.
Reprogramming sequence: power down (and bypass), write the dividers, wait at
least 500 ns, clear power down, poll bit 18 for lock.

Chain to the CPU and buses:

1. `sysclk` = PLL1 output if CLKSWCNTRL bit 1 = 1, otherwise the "alt sysclk"
   (source from CPUCLKCNTRL bits 15:14: 0 main, 1 DECT, 2 12 MHz).
2. CPU clock = sysclk / (CPUCLKCNTRL[3:0] + 1).
3. AHB clock (also the APB/peripheral clock, U-Boot's "PCLK") from the 2-bit
   field CPUCLKCNTRL[5:4]: 1 -> /2, 2 -> /3, 3 -> /4, and 0 behaves as /3.
   (The kernel computes this from the CPU clock and U-Boot from sysclk; they
   agree when the CPU divider is 0, as here.) Legal dividers are 2, 3, 4, and
   the vendor keeps hclk at or below 200 MHz.
4. On this product U-Boot always reprograms PLL1 to **400 MHz** assuming a
   25 MHz reference (R = 1, F = 16, OD = 0, band low) and sets the AHB divider
   to /3, so **hclk = 133.33 MHz**. It runs that sequence from the on-chip SRAM
   at 0x00200000 with the CPU temporarily on the alt clock. [src: U
   `cpu_speed_change`, `clk_reconfigure`, `reconfigure_cpu_speed`; log: "400
   MHz (CSS@400, PCLK@133)"]
5. The vendor kernel never changes the CPU or AHB rate (cpufreq is compiled
   out with the comment that the CPU shares sysclk with the peripherals).

CLKSWCNTRL (0x1C) switch bits:

| Bits | Function |
|---|---|
| 1 | sysclk source: 0 alt sysclk, 1 PLL1 |
| 2 | audio clock: 0 audio source, 1 PLL3 |
| 3 | slow clock enable |
| 14:4 | slow clock divider (main clock / (n+1)) |
| 15 | GPU source |
| 16 | APU source |
| 17 | LCD source: 0 sysclk, 1 PLL2 |
| 18 | CIU source |
| 20:19 | TDM1 source: 0 sysclk, 1 audio clock, 2 PLL2 |
| 22:21 | TDM2 source |
| 23 | Ethernet MAC TX clock select |
| 24 | Ethernet MAC RX clock select |
| 25 | Ethernet reference divider source: 0 sysclk, 1 PLL2 |
| 26 | QSPI source: 0 sysclk, 1 PLL2 |
| 27 | SD/MMC source |
| 28 | Ethernet RX clock pad select (output-enable style control) |
| 29 | Ethernet TX clock pad select |
| 31:30 | Ethernet TX pad clock source (0 ref divider, 1 internal divider, 2 and 3 phase-shifted ref) |

### 4.4 Gate and reset bits for the blocks asked about

Bit numbers 0..31 are in SWCLKENR1 / SWCHWRST1, 32..63 are bit (n-32) of
SWCLKENR2 / SWCHWRST2.

| Block | Clock gate bit | Reset bit | Parent / rate | Notes |
|---|---|---|---|---|
| UART1 | 6 | 6 | AHB | |
| UART2 | 7 | 7 | AHB | |
| timer1 | 32 | 32 (shared with timer2) | AHB | |
| timer2 | 33 | 32 | AHB | |
| timer3 | 34 | 33 (shared with timer4) | AHB | |
| timer4 | 35 | 33 | AHB | |
| RTC | 2 (+ CLKSWCNTRL bit 3 for the slow clock) | 2 | slow clock 1 MHz | |
| GPIO | none | none | always clocked | no entry in either tree |
| PLICU, SYSCFG, CMU | none | none | | |
| SPI1 (0x05D00000) | 38 | 38 | AHB | |
| SPI2 (0x05E00000, SLIC bus) | 39 | 39 | sysclk / (CLKDIVCNTRL3[31:24]+1) | U-Boot sets 2 MHz |
| TDM1 (0x05F00000) | 9 | 9 | mux CLKSWCNTRL[20:19], / (CLKDIVCNTRL4[11:0]+1) | U-Boot sets 2.048 MHz |
| TDM2 (0x06000000) | 10 | 10 | mux [22:21], / (CLKDIVCNTRL4[23:12]+1) | |
| GMAC | 3 (MAC RX and TX share it) | 3 | see section 6 | |
| RMII interface | 5 | none | | |
| Ethernet reference divider | 48 | none | sysclk or PLL2 / (CLKDIVCNTRL2[27:20]+1) | U-Boot only uses the low 4 bits |
| Ethernet internal divider | 49 | none | / (CLKDIVCNTRL2[5:0]+1) | |
| QSPI | 11 | 11 | sysclk or PLL2 / (CLKDIVCNTRL3[15:8]+1) | 50 MHz = 400/8 |
| DMA ("gdmac", 0x06400000) | 0 | 0 | AHB | |
| Watchdog (0x05700000) | none | none | | no entry in either tree |
| I2C1 / I2C2 | 36 / 37 | 36 / 37 | AHB | |
| Crypto (0x06C00000) | 4 | 4 | AHB | |
| SEC mailbox (0x00A00000) | 22 | 22 | AHB | |
| PMU / DVF9A (0x06300000) | 20 | 34 | AHB | |
| APU | 26 | 26 | | |
| CSS ARM9 core | 1 | 1 | | keep in reset if unused |
| CSS (bus) | 23 | 23 | | |
| CSS timers / CSS DMA | 45, 46 / 47 | 45 / 47 | | |
| NAND | 14 | 14 | AHB | |
| SD/MMC | 13 | 13 | | |
| LCD | 21 | 21 | | |
| MPMC (SDRAM controller) | 42 | 42 | AHB | **never gate or reset** |
| DRAM controller (Denali) | 44 | 44 | | **never touch** |
| CoreSight / ETM | 52, 27 | 62 | | |
| CCU0 | none | 52 | | |
| USB2 PHY / MAC | 17 / 16 | 57, 55, 54 | | |
| USB 1.1 | 41 | 41 | | |

What the vendor kernel does at start (`dvf_clk_init`), which a minimal kernel
gets for free only if it does the same:

1. Pulses the SD/MMC reset.
2. **Releases almost all block resets**: clears mask 0x0F7BFFDD in SWCHWRST1
   (everything except bits 1, 5, 18, 23, 28..31 = the CSS core and its
   satellites) and mask 0x003F0FF7 in SWCHWRST2 (everything except bits 3,
   12..15, 22, 23, 25, 30 = RF/APU, DRAM controller, CSS timers and DMA, USB2).
3. Points the clkout divider at the DECT oscillator.
4. Walks its clock tree and **turns off every gate that is on but unclaimed**,
   except clocks flagged "do not disable" (LCD, USB1.1, MPMC, the Ethernet
   reference and internal dividers, RMII) and their parents, and turns on ETM,
   CoreSight, DRAM controller and the CPU clock.

So in the vendor kernel the reset state of blocks is "whatever, then released",
and nothing guarantees a block is out of reset unless U-Boot or the kernel
released it. Reset default values of SWCHWRST1/2 are not in the tree (open
question 3).

### 4.5 What U-Boot (evb9918_ht) leaves configured at kernel entry

From U `arch/arm/lib/board.c`, `board/dspg/evb9918/*.c`,
`arch/arm/cpu/arm926ejs/dvf99/*.c`. All of this happens on every boot,
whatever is booted afterwards.

| Item | State at handoff | Where |
|---|---|---|
| PLL1 | 400 MHz, sysclk on PLL1 | `clk_reconfigure(400000000)` |
| AHB/APB clock | 133.33 MHz (divider /3) | same |
| Slow clock divider | set for 1 MHz. Gate (CLKSWCNTRL bit 3) **not** set by U-Boot | `clk_init` |
| Console UART (strap selected) | reset released, gate on, pads muxed, 115200 8N1, enabled, RX/TX watermarks 64 | `board_serial_init`, `serial_init_dev` |
| The other UART | untouched | |
| timer1 | reset released, gate on, free-running /16 32-bit, no IRQ | `timer_init` |
| timer2/3/4, RTC | untouched (timer2 shares the released reset with timer1 but its gate is off) | |
| QSPI | reset released, gate on, pads muxed, clock 33 MHz for probing then 50 MHz, controller enabled, memory-mapped read set up; flash is sent a software reset just before the jump | `board_qspi_init`, U `drivers/mtd/dspg-qspi.c`, `flash_exit` |
| SPI2 | reset released, gate on, 2 MHz, pads muxed | `board_spi_init` |
| TDM1 | reset released, 2.048 MHz, gate on, pads muxed, **block enabled** (CONFIG_1 = 0x1801, ENABLE = 1) | `board_tdm_init` |
| Ethernet | mode register set (RMII), reference divider 50 MHz, internal divider 25 MHz, gates 3, 5, 48, 49 on, MAC reset released, pads muxed, PHY hardware reset pulsed, PHY soft reset | `board_eth_init`, `clk_eth_config`, `gmac_eth_register` |
| D-class pad / APU | BGPIO27 handed to the D-class function | `board_apu_init` |
| DVF9A PMU | initialised | `dvf9a_init` |
| CCU0 | software-request mode enabled, dummy address pointing into U-Boot's memory | `ccu_init` |
| PLICU | untouched by U-Boot (no interrupts used) | |
| Watchdog | untouched; no watchdog code anywhere in the U-Boot tree | |
| GPIO, DMA | DMA untouched; GPIO only as listed in section 8 | |

Minimal-kernel conclusion: with fixed-clock nodes of **133 333 333 Hz** (AHB:
UART, timers, GMAC CSR clock), **50 MHz** (QSPI) and the Ethernet clocks as
left by U-Boot, a kernel can boot to a console, tick, and reach Ethernet and
the flash without a clock driver, provided it only uses the console UART,
timer1, QSPI, GMAC. Anything else (timer2, RTC, the second UART, DMA, SPI1,
I2C) needs its gate set and its reset released first. The exact values assume
the strap says 25 MHz; if the main oscillator were 50 MHz U-Boot's fixed
"25 MHz" assumption would give 800 MHz, which the log rules out for V1.x.

---

## 5. Memory map and boot protocol

### 5.1 Physical address map

From K `mach/hardware.h`, U `asm/arch-dvf99/hardware.h`, the dts.

| Address | Block | Address | Block |
|---|---|---|---|
| 0x00100000 | DDR controller (Denali), unused on this SDRAM board | 0x05A00000 | UART1 |
| 0x00200000 | on-chip SRAM, 32 KiB (last 256 bytes used by the DMA driver as memcpy scratch; first 0x100 mapped by the QSPI driver as bounce buffer) | 0x05B00000 | UART2 |
| 0x00300000 | USB2 OTG | 0x05D00000 | SPI1 |
| 0x00400000 | USB 1.1 | 0x05E00000 | SPI2 |
| 0x00600000 | GPU | 0x05F00000 | TDM1 |
| 0x00900000 | SD/MMC | 0x06000000 | TDM2 |
| 0x00A00000 | SEC (inter-CPU mailbox) | 0x06100000 / 0x06200000 | I2C |
| 0x01000000 | MPMC (SDRAM controller) | 0x06300000 | PMU / DVF9A interface |
| 0x01200000 | boot ROM | 0x06400000 | DMA controller |
| 0x05000000 | GPIO (+ EXTINT at 0x2A0, keypad at 0x2B0) | 0x06500000 | slave MII |
| 0x05100000 | PLICU | 0x06700000 | CIU |
| 0x05200000 | SYSCFG | 0x06B00000 | LCD controller |
| 0x05300000 | CMU | 0x06C00000 | crypto |
| 0x05400000 | NAND / LCD | 0x06E00000 | GMAC |
| 0x05500000 | QSPI controller | 0x07000000 / 0x07100000 | CCU0 / CCU1 |
| 0x05600000 | RTC | 0x07300000 | debug access port |
| 0x05700000 | watchdog | 0x08000000 | CSS ITCM (272 KiB) |
| 0x05800000 / +0x20 | timer1 / timer2 | 0x08100000 | CSS DTCM (128 KiB) |
| 0x05900000 / +0x20 | timer3 / timer4 | 0x08200000 | CSS AHB RAM (82 KiB) |
| 0x40000000, 0x60000000 | DDR windows (DVF9919 boards) | 0x80000000 | QSPI CS0 memory window (256 MiB decode) |
| 0xC8000000 | **SDRAM, 32 MiB** | 0xA0000000 | QSPI CS1 memory window |

The vendor kernel statically maps only CMU and SYSCFG, at virtual
0xFE000000 + (phys - 0x05000000). SYSCFG CHIP_ID (0x1C) reads 0x9910; CHIP_REV
(0x20) reads 16 on the "BBC" silicon seen in the V1.x log.

### 5.2 RAM and the 0xC9800000..0xC9CFFFFF gap

- RAM: 32 MiB at 0xC8000000..0xC9FFFFFF. U-Boot sizes it from the MPMC
  dynamic-config register (0x01000120, bits 11:9: 0 = 2, 1 = 8, 2 = 16,
  3 = 32, 4 = 64 MiB). [src: U `dram_init`]
- The DTB in flash says 16 MiB (`reg = <0xc8000000 0x1000000>`). U-Boot
  rewrites the memory node to the detected size before starting the kernel.
- **The gap is the CSS (DSP core) shared/DMA memory, and it is carved out by
  the vendor kernel, not by the hardware or the boot loader.** At early boot
  the vendor kernel allocates CONFIG_DVF_RESERVEMEM_CSS mebibytes, 1 MiB
  aligned, from memblock and removes them from the memory map; the CSS loader
  later claims that block as its exclusive coherent DMA pool. [src: K `arch.c`
  `dvf_reserve`, `dvf_dev_reserve_dmamem`; K `drivers/staging/dspg/css.c` probe]
- Why it lands exactly at 0xC9800000: memblock allocates top-down, U-Boot has
  parked the relocated DTB at about 0xC9D2C000 (reserved), the space above the
  DTB is under 5 MiB, so the highest free 5 MiB block, aligned down to 1 MiB,
  is 0xC9800000..0xC9CFFFFF. [infer, but the arithmetic matches both this
  unit (5 MiB hole) and the V1.x log (3 MiB reserved, "26MB 3MB = 29MB").]
- The value is 8 in the GPL `defconfig_ht80x`, 3 in the 1.0.0.24 log and 5 on
  this unit's 1.0.65 kernel, so it is a per-release kernel config.
- Consequence: a kernel that does not run the CSS core has nothing to reserve
  there. All 32 MiB are ordinary RAM.
- The vendor kernel also sets the coherent DMA pool to 14 MiB (`dvf_map_io`).

### 5.3 Boot chain

boot ROM -> Bootastic (flash offset 0, 16 KiB) -> U-Boot (one of two copies) ->
`bootcmd=gsboot` -> kernel.

- Bootastic picks between the two U-Boot copies: strings in this unit's
  bootastic are "Found a valid GS uboot image", "Using uboot partition image",
  "Using uboot2 partition image", "No GS uboot image found". Its selection rule
  is not in the tree (no Bootastic source). [unit]
- U-Boot runs at 0xC8400000 before relocation, relocates itself to the top of
  RAM. Its payload inside the partition is the Grandstream 512-byte header
  followed by a DSPG loader header and the binary (load address 0xC8400000
  visible in the header). [unit]
- U-Boot environment: 8 KiB at flash offset 0xFBE000. On this unit it is
  erased (all 0xFF), so the **compiled-in default environment** is used on
  every boot ("bad CRC, using default environment"). [unit]

### 5.4 Kernel image format expected by `bootm`

- **Legacy uImage** (magic 0x27051956), OS Linux, arch ARM, type kernel,
  **compression none**, load address = entry point = **0xC8008000**. The
  payload is an ordinary self-extracting **zImage** (the stock one is LZMA
  inside). [unit: header at core + 0x200; log: "ARM Linux Kernel Image
  (uncompressed)"]
- U-Boot has gzip/zlib image support compiled out (`#undef CONFIG_GZIP`,
  `CONFIG_ZLIB`) and no LZMA/LZO/bzip2, so a compressed uImage will not boot.
  Compress inside the zImage. [src: U `include/configs/dvf99.h`]
- `bootm` verifies the uImage header CRC and data CRC (stock "Verifying
  Checksum ... OK").
- FIT images: not enabled.
- Size limit when booted from flash: the core partition is 1280 KiB including
  the 512-byte Grandstream header; the stock uImage is about 1.27 MB used of
  it (SizeUsed 0x135E00). When booted from RAM (TFTP/serial) the limit is only
  the distance between the staging address 0xC8800000 and the DTB staging
  address 0xC8C00000 (4 MiB) unless other addresses are chosen.
- The uImage is staged at `loadaddr` = 0xC8800000 and copied by `bootm` to
  0xC8008000. After the copy U-Boot cleans the whole D-cache.

### 5.5 Kernel entry conditions (important, see also 9.1)

- r0 = 0, r1 = machine id, r2 = address of the (relocated) DTB when a DTB is
  given as third `bootm` argument, otherwise ATAGs at 0xC8000100.
- Machine id: the registered number is 4279 (MACH_DVF99), but this U-Boot
  config defines no CONFIG_MACH_TYPE, so r1 is whatever the unset board field
  holds (expected 0) unless the `machid` environment variable is set. The
  vendor kernel is device-tree only (`dt_compat = "dspg,dvf99"`), no ATAG
  machine. [src]
- DTB handling by U-Boot: the blob at `devtree_addr` gets its `/chosen`
  bootargs and memory node written, is grown by 0x3000 bytes of padding and
  relocated near the top of RAM (0xC9D2C000 in the log). The vendor builds its
  DTBs with `dtc -R 8 -p 0x3000`. [src: K `build.sh`; log]
- Extra non-standard ATAG "gxp_console" exists only on the ATAG path.
- **`gsboot` enables the MMU and D-cache before `bootm`, and this U-Boot does
  not turn them off again**: the usual "cleanup before Linux" call is commented
  out. Entry state via `gsboot`: MMU on with a flat 1:1 map of the whole 4 GiB
  in 1 MiB sections (RAM and 0x80000000..0x80FFFFFF cacheable and bufferable,
  everything else uncached), D-cache on, I-cache on, interrupts never enabled.
  Entry state via a manual `bootm` at the prompt without `gsboot`: MMU off,
  D-cache off, I-cache on (enabled in `board_init`). [src: U
  `common/cmd_gsboot.c` `do_gsboot`, `arch/arm/lib/bootm.c`
  `announce_and_cleanup`, `arch/arm/lib/cache-cp15.c` `mmu_setup`,
  `board/dspg/evb9918/evb9918.c` `board_init`]

### 5.6 Stock bootargs

Built by `gsboot` (GPL source and this unit's U-Boot strings agree):

```
${gxp_console} root=/dev/mtdblock<6|7> bootpartition=<0|1> mtdparts=qspi:<table>
```

- `${gxp_console}` expands to `gxp_console=0` by default. The shipped binary
  also contains the string `gxp_console=1`, which the GPL tree does not (the
  shipped default environment has `console=no`; GPL code only handles
  `console=yes`).
- There is **no `console=` argument and no `rootfstype`**. The vendor kernel
  gets its console from its built-in config. A mainline kernel must bring its
  own (`stdout-path`, CONFIG_CMDLINE).
- Slot A selected: `root=/dev/mtdblock6 bootpartition=0` and the table in 7.4.
  Slot B selected: `root=/dev/mtdblock7 bootpartition=1` and the same table
  with every A/B name pair swapped (7.4).
- `gsboot nfs` variant uses `${linux_bootargs_nfs}` = `root=/dev/nfs ip=dhcp
  nfsroot=192.168.0.240:/target rw` instead of root/bootpartition.

### 5.7 Grandstream partition header and how `gsboot` selects

Each of uboot, core, base, prog (both copies) starts with a 512-byte header,
little endian. Field names are the ones used in U `common/cmd_gsboot.c`; the
structure definition itself (`provision.h`) is missing from the GPL release, so
the offsets below were recovered from this unit's flash and **verified** (the
checksum rule reproduces on core, core2, base, prog and uboot).

| Offset | Size | Field | Example (core) |
|---|---|---|---|
| 0x00 | 4 | Signature = 0x23C97AF9 | F9 7A C9 23 on flash |
| 0x04 | 4 | Version, one byte per component, major in the top byte | 0x01004101 = 1.0.65.1 |
| 0x08 | 4 | partition/image capacity | 0x00140000 |
| 0x0C | 4 | SizeUsed = payload length in bytes | 0x00135E00 |
| 0x10 | 2 | image type id (0x0B uboot, 0x0C core, 0x0D base, 0x0E prog) | 0x000C |
| 0x12 | 2 | Checksum | 0x5A08 |
| 0x14 | 4 | build date: year (16 bit), month, day | 2025-10-12 |
| 0x18 | 4 | probably build time, not decoded | |
| 0x1C | 4 | 1 (meaning unknown) | |
| 0x24 | 4 | 0x00007FFF (hardware mask, presumably) | |
| 0x28 | 4 | CompatibleVersion[0] | 1.0.65.1 |
| 0x2C | 4 | CompatibleVersion[1] | 1.0.65.1 |
| 0x30 | 4 | ProvisionCounter | 3 (slot B), 2 (slot A) |
| 0x34 | 16 | ASCII tag: "dvf9918" on uboot and core, "any" on base and prog | |
| 0x54 | 4 | 1 (meaning unknown) | |
| 0x200 | | payload (for core: the uImage) | 27 05 19 56 ... |

Checksum rule: add up the payload (bytes 0x200 .. 0x200+SizeUsed) as
little-endian 16-bit words, modulo 65536; the image is good when that sum plus
the Checksum field is 0 modulo 65536. The header itself is not covered. Space
after the payload is 0xFF. [src: `calculate_checksum`, `check_part`; unit]

Validation per partition (`check_part`): signature equals the magic, checksum
passes. Nothing else. No signature, no hash, no hardware-mask check in U-Boot.

Selection (`check_parts`, called once from `gsboot`). "Set A" = core, base,
prog; "set B" = core2, base2, prog2, by U-Boot's own partition names (7.4).

1. All six partitions validated.
2. If all six are valid:
   a. A set is "compatible" when prog's CompatibleVersion[0] equals core's
      Version and CompatibleVersion[1] equals base's Version.
   b. Exactly one set compatible: that set wins.
   c. Otherwise the set with the higher ProvisionCounter wins (per set, the
      maximum over its three headers).
   d. If equal, the higher Version wins, comparing prog first, then base, then
      core. All equal: set A.
3. Not all valid: first match of: A's three valid -> A; B's three valid -> B;
   both cores and both bases valid (progs bad) -> counter, then base version,
   then core version; A's core+base valid -> A; B's core+base valid -> B;
   otherwise A.

Then it builds the bootargs, copies the chosen core partition minus the first
0x200 bytes to 0xC8800000, copies the `factory` partition from offset 0x4000
(the DTB) to 0xC8C00000, and runs `bootm 0xC8800000 - 0xC8C00000`.

**Fallback behaviour:** the fallback to the second copy happens **only at
validation time** (bad magic or bad checksum in the Grandstream header).
There is no boot counter, no "boot succeeded" flag and no watchdog in U-Boot.
A kernel with a valid Grandstream header that then hangs, panics, or fails the
uImage CRC is **not** retried from the other copy: `bootm` either never
returns or returns to `gsboot`, which returns to the U-Boot prompt. [src]

State of this unit: slot A headers carry counter 2, slot B headers counter 3
(and prog B is 1.0.65.2), so U-Boot boots **set B** (physical core at
0x1A0000) with `bootpartition=1`. That is why `/proc/mtd` shows `uboot2`,
`core2` before `uboot`, `core`: Linux's names are swapped so that the un-suffixed
names always mean the running set. [unit]

Practical consequence for a custom kernel in flash: it needs the 512-byte
header with a correct checksum, and its set has to win the comparison above.
The safest experiment keeps the other set intact and valid; it will be used
automatically if the new image's header is damaged, but not if the new kernel
merely fails to boot (recovery then needs the U-Boot prompt or an external
programmer).

### 5.8 Device tree location in flash

`factory` partition (flash 0x4000..0xFFFF, 48 KiB): board parameters at the
start (the MAC address is at factory offset 0x06, 6 bytes, c0:74:ad:xx:xx:xx
on this unit), DTB at partition offset 0x4000 = flash offset 0x8000 (magic
D0 0D FE ED there). [unit] The DTB is shared by both slots.

### 5.9 U-Boot commands for loading and starting from RAM

Present in the GPL config and confirmed by help strings in this unit's U-Boot
binary:

| Command | Use |
|---|---|
| `tftpboot` / `tftp <addr> <file>` | TFTP load. Defaults: ipaddr 192.168.0.240, serverip 192.168.0.1, netmask /24, loadaddr 0xC8800000. `CONFIG_TFTP_PORT` is on (env `tftpsrcp`/`tftpdstp` usable) |
| `dhcp`, `bootp`, `ping` | present |
| `loady [addr] [baud]` | YMODEM over the console |
| `loadb [addr] [baud]` | Kermit over the console |
| `bootm <uImage addr> - <dtb addr>` | start a legacy uImage with a DTB |
| `go <addr>` | jump to raw code |
| `qspi read.partial <part> <dest> [offset]` | copy a whole partition (minus offset) to RAM |
| `qspi write.partial <part> <src> <len>` | erase the whole partition, then program `len` bytes |
| `qspi write.mpartial <first> <last> <src> <len>`, `qspi write <src> <offset> <len>`, `qspi info` | |
| `gsboot`, `gsboot nfs` | the stock boot |
| `run tftp_boot` | `load_cfg; tftp core.${GSMODEL}.nfs; tftp ${MODEL}_${PNSTR}.dtb; run bootm` |
| `run flash_boot` | read core (+0x200) and the factory DTB, then `run bootm` |
| `md`, `mw`, `cp`, `cmp`, `crc32`, `mtdparts`, `printenv`, `setenv`, `saveenv`, `nvram`, `fdt`, `iminfo`, `reset`, `test_timers` | present |

Not present: `loads`, `source`, `nfs`, `md5sum`, `imxtract`, `i2c`, `sspi`
(explicitly removed to save space).

RAM boot recipe that follows from the above (no flash write): load a legacy
uImage to 0xC8800000 and a padded DTB to 0xC8C00000, set `bootargs`, then
`bootm 0xC8800000 - 0xC8C00000`. Run this way (not through `gsboot`) the
kernel is entered with the MMU off.

Console caveat: in the GPL tree the HT build defines CONFIG_SERIAL_UNLOCKED,
so serial input always works and Esc during the 1 s countdown gives the prompt
(abort key is Esc only, 0x1B). The same source has a lock for other products:
all serial input and `puts` output are suppressed until the environment
variable `console` equals `yes` (checked right after the environment is
loaded). This unit's shipped U-Boot has `console=no` in its default
environment, which the GPL HT config does not, so the shipped build may have
the lock active (open question 1). [src: U `serial.c`, `board.c` near the
"check serial lock" comment; strings]

---

## 6. Ethernet

Source: K `drivers/net/ethernet/stmicro/stmmac/` (vendor-modified),
K `arch/arm/mach-dvf/clock.c` (`dvf_eth_config`, `clk_update_etherdiv`),
K `arch/arm/mach-dvf/fixups.c`, U `board/dspg/evb9918/ethernet.c`,
U `drivers/net/gmac.c`, U `arch/arm/cpu/arm926ejs/dvf99/clock.c`.

### 6.1 MAC

- Synopsys DesignWare GMAC, "user ID 0x20, Synopsys ID 0x37" (core 3.7x) in
  the V1.x log, with the DMA hardware-capability register present.
- Base 0x06E00000, size 0x8000, one interrupt: PLICU source 23, named
  "macirq". No separate wake or LPI interrupt.
- Vendor platform settings (matched as `st,spear600-gmac`): GMAC core,
  PBL = 8, PMT flagged, ring-mode descriptors (not chained). No other tuning
  (no fixed burst, no AAL, no force-store-and-forward flags). [src:
  `stmmac_probe_config_dt`, defconfig]
- MDIO clock range: the vendor dts gives `clk-csr = <150000000>` (a frequency
  in the vendor binding), which selects the 150..250 MHz range. The real CSR
  clock is the AHB clock, 133.33 MHz, i.e. the 100..150 MHz range.

### 6.2 PHY interface mode register

SYSCFG (0x05200000) **GCR1 at offset 0x04**:

| Bits | Meaning |
|---|---|
| 7:5 | MAC interface mode: 0 = MII (also slave MII), 4 = RMII (also slave RMII), 1 = RGMII |
| 1 | route the main clock to the BGPIO31 pad ("main clock out"); set by U-Boot for this product together with releasing BGPIO31 from GPIO mode |

The mode field **must be written before the MAC reset (reset bit 3) is
released**. [src: U `board_eth_init` comment and order] The vendor kernel never
writes this register; it relies on U-Boot.

### 6.3 Ethernet clocks

Per-mode settings, identical in both trees ("Table 1 from CMU specification"):

| Mode | TX clk sel (CLKSWCNTRL bit 23) | RX clk sel (bit 24) | TX pad source (bits 31:30) | TX pad bit 29 | RX pad bit 28 | internal divider used | reference rate |
|---|---|---|---|---|---|---|---|
| MII | 0 | 0 | 0 | 0 | 1 | no | 25 MHz |
| slave MII | 1 | 0 | 0 | 0 | 1 | no | 25 MHz |
| **RMII** | 1 | 1 | 0 | 0 | 1 | yes | **50 MHz** |
| slave RMII | 1 | 1 | 0 | 0 | 1 | yes | 50 MHz |
| RGMII | 1 | 0 | 3 | 0 | 1 | yes | 125 MHz |

RMII with the reference clock generated by the SoC ("internal", which is what
this unit's factory data says):

1. Reference divider source: try PLL2 (CLKSWCNTRL bit 25 = 1), fall back to
   sysclk (bit 25 = 0). From sysclk 400 MHz the divider field
   CLKDIVCNTRL2[23:20] = 7 gives exactly 50 MHz.
2. CLKDIVCNTRL1 bit 23 = 0 (RMII clock from the internal reference), gate 48 on.
3. Write the five switch fields from the table.
4. Gate 49 on; internal divider CLKDIVCNTRL2[5:0] = (50 MHz / MAC clock) - 1:
   **1 for 100 Mbit/s (25 MHz), 19 for 10 Mbit/s (2.5 MHz)**.
5. Release nothing for "rmii_if" (it has no reset bit), gate 5 on; release MAC
   reset bit 3, gate 3 on.

RMII with an external 50 MHz reference ("external" in factory data):
CLKDIVCNTRL1 bit 23 = 1, gate 48 off, and CLKSWCNTRL bit 29 = 1.

**Speed change needs a clock write.** On a link change the vendor MAC driver
rewrites only the internal divider (CLKDIVCNTRL2[5:0]) for 10 or 100 Mbit/s;
it is hard-coded for RMII and warns that 1000 is unsupported. [src:
`stmmac_adjust_link` -> `clk_update_etherdiv`] A mainline `snps,dwmac` node
with no glue will work at 100 Mbit/s as U-Boot left it and will not pass
traffic at 10 Mbit/s.

### 6.4 Pads

Ethernet pins are EGPIO0..17, switched to the MAC by clearing their GPIO-enable
bits and setting the "emaca" pad group (SYSCFG SPCR1 bit 14):

| Pad | Signal | Pad | Signal |
|---|---|---|---|
| EGPIO0, 1 | TXD0, TXD1 | EGPIO8 | MDIO |
| EGPIO2 | TXEN | EGPIO9 | MDC |
| EGPIO3, 4 | RXD0, RXD1 | EGPIO10 | TXER in MII; **PHY reset GPIO in the GPL ht config** |
| EGPIO5 | CRS_DV | EGPIO11, 12 | TXD2, TXD3 (driven low as GPIO outputs in RMII) |
| EGPIO6 | RXER (left as GPIO input in RMII) | EGPIO13, 14 | RXD2, RXD3 |
| EGPIO7 | REFCK (50 MHz) | EGPIO15 | RXCLK |
| | | EGPIO16, 17 | CRS, COL (MII only; these are also UART2 location 0) |

### 6.5 PHY: what the trees say and what they do not

- The GPL U-Boot config for this product is for the **older KSZ8081** board:
  RMII, PHY at **MDIO address 0**, hardware reset on **EGPIO10** (low 15 ms,
  then high, 150 ms wait), then a soft reset through BMCR, a write clearing
  bit 15 of PHY register 0x16, and for the internal-clock case setting bit 7
  of PHY register 0x1F (KSZ8081: 50 MHz reference input mode). [src]
- **Nothing in either tree mentions Motorcomm or YT8512.** The mainline
  6.6.63 tree in `~/v90d/fw/build` has no YT8512 entry in
  `drivers/net/phy/motorcomm.c` either (only YT8511, YT8521, YT8531, YT8531S),
  so the generic PHY driver would bind.
- The YT8512H is a 10/100 PHY with MII/RMII only. `HARDWARE.md` records
  `eth_interface=RGMII` from the unit's hw_features; the vendor code that
  prints that field only knows the two labels RGMII and RMII, and the enum
  values are in the missing `provision.h`. The dts on the unit says
  `phy-mode = "rmii"` and the stock link handler is hard-coded RMII. I would
  treat the unit as RMII with SoC-generated reference clock until GCR1 is read
  on the box (open question 5).
- The vendor kernel does not attach to the real PHY through the device tree:
  the dts has `fixed-link = <1 1 100 0 0>` (fixed PHY id 1, full duplex,
  100 Mbit/s) and an `mdio { reg = <0>; }` stub; `phy_addr` is left at -1
  (auto) in the platform data. A board fixup registers the fixed PHY at
  arch-init time. [src: `fixups.c` `of_add_fixed_phys`]
- The vendor kernel takes the MAC address from the factory partition
  (offset 0x06), not from the DTB (`mac-address` is all zero there).

### 6.6 Vendor changes inside stmmac worth knowing

- Explicit `wmb()` calls around every "prepare descriptor / hand ownership to
  DMA" step in the transmit path. On this platform `wmb()` is overridden to
  include a memory-controller flush (section 9.4). Whether stock 3.4 already
  had those calls I did not check; the point is that the vendor wanted
  descriptor writes forced out to RAM before the MAC's DMA looks at them.
- Link LED: the RJ45 LED GPIO is driven from the link-change handler.

### 6.7 What a mainline `snps,dwmac` node needs

- `compatible = "snps,dwmac-3.70a", "snps,dwmac"`, `reg = <0x06e00000 0x8000>`,
  `interrupts = <23>`, `interrupt-names = "macirq"`.
- A clock named `stmmaceth` at 133 333 333 Hz (fixed clock is enough) so the
  driver derives the MDC divider; or `snps,clk-csr` set for the 100..150 MHz
  range.
- `phy-mode = "rmii"`, `snps,pbl = <8>`.
- An `mdio` child (`snps,dwmac-mdio`) with the PHY. Address 0 is what the GPL
  board used; unverified for the YT8512H (open question 5).
- PHY reset GPIO: EGPIO10 on the GPL board; unverified for V2.5A. U-Boot has
  already pulsed it, so leaving it out is safe for a first boot.
- `nvmem` cell or boot-time fixup for the MAC address (factory offset 0x06).
- Something for the 10 Mbit/s clock change (6.3), or accept 100 only.
- Nothing is needed for mode/pads/clocks on a normal boot because U-Boot's
  Ethernet init runs on every boot ("Net: RMII, gmac0@100Mbps" in the log)
  before the autoboot countdown.

---

## 7. QSPI flash controller (0x05500000)

Source: K `drivers/mtd/devices/dspg-qspic.c`, U `drivers/mtd/dspg-qspi.c`,
U `board/dspg/evb9918/qspi.c`.

### 7.1 Resources

dts: `reg = <0x05500000 0x800>, <0x00200000 0x100>, <0x80000000 0x10000000>,
<0xa0000000 0x10000000>`; interrupt = PLICU source 16.

1. controller registers;
2. the first 256 bytes of the on-chip SRAM, used by the vendor driver as a
   word-aligned bounce buffer for programming;
3. CS0 memory window (the flash, 16 MiB used, at 0x80000000);
4. CS1 memory window (nothing fitted on this board).

Clock: CMU gate 11, reset 11, divider CLKDIVCNTRL3[15:8], source CLKSWCNTRL
bit 26. Probe runs at 33 MHz, then the driver switches to the table rate for
the chip (50 MHz for the GD25Q128C).

### 7.2 Registers

Global:

| Offset | Name | Notes |
|---|---|---|
| 0x000 | CFG | vendor value 0x10009 = controller enable, SPI mode 3, long chip-select deselect time |
| 0x008 | STS | bit 31 busy, bit 0 idle |
| 0x00C | ISR | interrupt status; bit 6 = "APB transfer done" |
| 0x010 | IER | interrupt enable; vendor enables bit 6 only |
| 0x014 | ICR | interrupt clear; **not self-clearing**: write bit 6, wait until ISR bit 6 drops, then write 0 |
| 0x020 .. 0x03F | FWD | 32-byte write data buffer for register-driven commands |
| 0x040 .. 0x05F | FRD | 32-byte read data buffer |
| 0x300 | TRSTP | transfer setup for a register-driven ("aux") command |
| 0x304 | CMD | command word; writing it with bit 31 starts the transfer |
| 0x308 | ADD | address for the aux command |
| 0x408 | CSTIMING | vendor value 0x300 ("enlarged CS timing") |

Per chip select (CS0 block at 0x100, CS1 block at 0x200):

| Offset in block | Name | Notes |
|---|---|---|
| 0x3C | RDTRSTP | how memory-window **reads** are performed |
| 0x40 | RDCMD | read opcode |
| 0x44 | PPTRSTP | how memory-window **writes** (page program) are performed |
| 0x48 | PPCMD | program opcode |
| 0x54 | MEMADD | vendor writes 0xFFFF0000 ("we verify ourselves") |

Setup-word fields. Line-count codes: 0 = single line, 1 = dual, 2 = quad.
Address-size codes: 1 = 3 bytes, 2 = 4 bytes, 0 = no address.

| Field | TRSTP (aux) | RDTRSTP (read) | PPTRSTP (program) |
|---|---|---|---|
| opcode lines | bits 1:0 | bits 1:0 | bits 1:0 |
| address lines | bits 3:2 | bits 3:2 | bits 3:2 |
| dummy lines | bits 5:4 | bits 5:4 | bits 5:4 |
| data-out lines | bits 7:6 | | bits 7:6 |
| data-in lines | bits 9:8 | bits 9:8 | |
| address size | bits 11:10 | bits 11:10 | bits 11:10 |
| dummy size | | from bit 12 | |
| "has opcode" | bit 19 | bit 19 | bit 19 |
| "has mode byte" | | bit 20 | |
| data byte count (aux) | from bit 20 | | |

CMD word: bits 7:0 opcode; bits 12:8 command type (0 sector erase, 1 chip
erase, 5 read status, 6 write status, 9 other); bit 13 = data phase is a read;
bit 14 = data phase is a write; bit 30 = chip select (0 or 1); bit 31 = START.

### 7.3 How the operations are done

**Read**: once RDTRSTP/RDCMD are programmed, the flash content appears in the
CS memory window; the vendor MTD read is a plain memory copy from 0x80000000 +
offset. The boot ROM and U-Boot use the same window (U-Boot's flash "bank"
starts at 0x80000000).

**Register-driven ("aux") command** (JEDEC ID, status, write enable, erase):
1. Wait until STS shows not busy and idle.
2. For a write-data command, fill FWD first.
3. Write TRSTP (line configuration | has-opcode | byte count << 20; address
   size field cleared when the command has no address).
4. For an addressed command write ADD.
5. Write CMD = opcode | type | direction | (cs << 30) | bit 31.
6. Wait for ISR bit 6 (interrupt or poll), clear it as described above.
7. For a read-data command, take the bytes from FRD.

Buffer byte order: both buffers are filled **from the end backwards**. The
first byte on the wire is at offset +31 of the 32-byte buffer, the next at
+30, and so on. Maximum 32 data bytes per aux command.

**Erase a 4 KiB sector**: write enable (opcode 0x06, type other), poll status
(0x05, type read-status, 1 byte) until WEL (bit 1) is set; ADD = flash offset;
CMD = 0x20, type sector-erase; then poll status until both busy (bit 0) and
WEL (bit 1) are clear. Vendor timeout 4 s. Chip erase = 0xC7, type 1. 64 KiB
block erase = 0xD8, type 0 (defined, unused). The vendor MTD advertises a
4 KiB erase size.

**Program**: there is no aux page-program. Programming is done by **writing
32-bit words into the memory window** at the target offset; the controller
turns the burst into a page-program using PPTRSTP/PPCMD. Vendor procedure per
chunk (never crossing a 256-byte page):
1. copy the data into the SRAM bounce buffer and pad to a multiple of 4 bytes
   with the bytes currently in flash;
2. write enable and wait for WEL;
3. **with interrupts disabled**, copy the words from the bounce buffer to the
   window in one uninterrupted loop. The vendor comment says the write ends
   prematurely if the controller's FIFO runs empty, so the burst must not be
   interrupted;
4. poll status until busy and WEL are clear.

**Identification**: opcode 0x9F, type other, read 5 bytes: 3 JEDEC bytes + 2
extended. If nothing known answers in single-line mode the vendor driver
retries the ID read with all phases on four lines (in case the chip is still in
QPI mode).

### 7.4 This board's flash and its mode

- GigaDevice GD25Q128C, JEDEC 0xC84018, 16 MiB, 256-byte pages, 4 KiB sectors.
- Both vendor drivers run it in **QPI mode** (opcode, address and data all on
  four lines): read opcode 0x0B with 3-byte address and dummy-size code 2,
  program opcode 0x02, aux commands on four lines. Entry sequence: read status
  registers (0x05, 0x35, 0x15); if the QE bit (bit 1 of status register 2) is
  clear, write enable and write 0x02 to status register 2 with opcode 0x31 and
  wait for busy to clear; then send opcode 0x38 (enter QPI). [src:
  `dspg_qspic_init_gd25q128_qpi` in both trees]
- **U-Boot leaves QPI before starting the kernel**: just before the jump it
  sends reset-enable 0x66 and reset 0x99, which returns the chip to single-line
  SPI. The controller's per-CS read setup still says "QPI, opcode 0x0B", so at
  kernel entry **the memory window is not valid** until a driver reprograms the
  chip or the controller. [src: U `flash_exit`, called from
  `announce_and_cleanup`]
- The vendor kernel never leaves QPI on shutdown; [infer] the boot ROM copes
  either way (it starts by loading Bootastic from the same chip after every
  software reset).
- A simple mainline MTD driver can stay in single-line mode: read opcode 0x03
  or 0x0B on one line through RDTRSTP/RDCMD, program 0x02 on one line, aux
  commands on one line. Slower, but no QE/QPI handling. Not verified on
  hardware; all vendor usage is quad.

### 7.5 Partition table

Physical layout (same in U-Boot's default environment, the stock bootargs, and
this unit). Offsets confirmed by the V1.x kernel log.

| # | Offset | Size | U-Boot name (slot A view) | Linux name when slot B runs (this unit) |
|---|---|---|---|---|
| 0 | 0x000000 | 16 KiB | bootastic | bootastic |
| 1 | 0x004000 | 48 KiB | factory | factory |
| 2 | 0x010000 | 64 KiB | nvram | nvram |
| 3 | 0x020000 | 256 KiB | uboot | uboot2 |
| 4 | 0x060000 | 1280 KiB | core | core2 |
| 5 | 0x1A0000 | 1280 KiB | core2 | core |
| 6 | 0x2E0000 | 3072 KiB | base | base2 |
| 7 | 0x5E0000 | 3072 KiB | base2 | base |
| 8 | 0x8E0000 | 3324 KiB | prog | prog2 |
| 9 | 0xC1F000 | 3324 KiB | prog2 | prog |
| 10 | 0xF5E000 | 128 KiB | lang | lang2 |
| 11 | 0xF7E000 | 128 KiB | lang2 | lang |
| 12 | 0xF9E000 | 128 KiB | oem | oem |
| 13 | 0xFBE000 | 8 KiB | uboot_env | uboot_env |
| 14 | 0xFC0000 | 256 KiB | uboot2 | uboot |

- The table reaches Linux only through the `mtdparts=qspi:...` argument; the
  MTD device name is "qspi". The dts has no partition nodes.
- Slot A string: `16k(bootastic),48k(factory),64k(nvram),256k(uboot),1280k(core),1280k(core2),3072k(base),3072k(base2),3324k(prog),3324k(prog2),128k(lang),128k(lang2),128k(oem),8k(uboot_env),-(uboot2)`.
  Slot B string: the same sizes with each pair of names exchanged. The mtd
  numbers stay fixed; only names move. Root is mtdblock6 for slot A and
  mtdblock7 for slot B.
- U-Boot itself always uses the slot A names, so `qspi read.partial core ...`
  at the prompt reads offset 0x060000 whichever slot Linux calls "core".

---

## 8. Pin multiplexing

Source: K `drivers/gpio/gpio-dspg.c`, K `include/linux/gpio-dspg.h`,
K `mach/hardware.h`, U `arch/arm/cpu/arm926ejs/dvf99/gpio.c`, `pads.c`,
U `board/dspg/evb9918/*.c`.

There is no pinctrl driver in the vendor kernel. Two mechanisms together decide
what a pad does.

### 8.1 Per-pin GPIO enable (GPIO block, 0x05000000)

Five banks A..E, 32 pins each, register block per bank at 0x60 stride (A at
0x000, B at 0x060, C at 0x0C0, D at 0x120, E at 0x180). One bit per pin.
Global GPIO number = bank x 32 + pin (160 GPIOs).

| Offset in bank | Register | Use |
|---|---|---|
| 0x00 | DATA | read pin value (the vendor `get` reads this) |
| 0x04 / 0x08 | DATA_SET / DATA_CLR | write 1 to drive high / low |
| 0x0C | DIR | read: 1 = input, 0 = output |
| 0x10 / 0x14 | DIR_OUT / DIR_IN | write 1 to make output / input |
| 0x18 | EN | read: **1 = pad is a GPIO, 0 = pad belongs to its peripheral function** |
| 0x1C / 0x20 | EN_SET / EN_CLR | write 1 to select GPIO / peripheral |
| 0x24 | PULL_CTRL | read: pull resistor enabled |
| 0x28 / 0x2C | PULL_EN / PULL_DIS | |
| 0x30 | PULL_TYPE | read: 1 = pull-up, 0 = pull-down |
| 0x34 / 0x38 | PULL_UP / PULL_DOWN | |
| 0x3C | KEEP | read: bus keeper |
| 0x40 / 0x44 | KEEP_EN / KEEP_DIS | |
| 0x48 | OD | read: open drain |
| 0x4C / 0x50 | OD_SET / OD_CLR | |
| 0x54 | IN | defined, unused |

All set/clear registers are write-one, so no read-modify-write and no locking
is needed for pin control.

Pins that do not exist (dts `dspg,nx_masks`): AGPIO14, 15, 20..25, 30, 31;
BGPIO4..7, 15, 27..31; DGPIO27..29. (BGPIO27 and BGPIO31 are in that mask yet
U-Boot switches them to their peripheral function for D-class and clock out,
so "not usable as GPIO" is the accurate reading.)

Vendor behaviour worth copying or avoiding: `request` forces the pin into GPIO
mode if it was not; Grandstream's `free` switches the pin back to peripheral
mode. Setting direction also disables the pull resistor on push-pull outputs
and on inputs that have the keeper enabled.

DT GPIO specifier in the vendor binding: three cells, (bank, pin, flags).

### 8.2 Pad group enables (SYSCFG, 0x05200000)

A peripheral's pads also need its group bit set. SPCR1 at 0x54, SPCR2 at 0x58.

| SPCR1 bit | Group | SPCR1 bit | Group |
|---|---|---|---|
| 31 | RF/APU | 17, 16 | I2C1, I2C2 |
| 30 | MLSE | 15 | SD/MMC |
| 29 | PMU | 14 | **Ethernet MAC ("emaca")** |
| 28 | digital mic | 12 | BMP sync port |
| 27 | NAND ("fc") | 11 | slave MII |
| 26 | **UART1** | 10 | LCD controller |
| 25 | **UART2** | 8..0 | LCD GPIO pads 8..0 |
| 24 | UART2 pad location (0 = EGPIO16/17, 1 = AGPIO28/29) | | |
| 23, 22 | **TDM1**, TDM2 | | |
| 20, 19 | SPI1, **SPI2** | | |

| SPCR2 bit | Group | SPCR2 bit | Group |
|---|---|---|---|
| 19 | trace port | 7 | D-class |
| 18 | SD/MMC data location | 6 | BMP |
| 17, 16 | EXTINT13, EXTINT14 pad location | 5 | USB VBUS drive |
| 14..8 | memory address lines A25..A21, A18, A17 | 4 | clock out |
| | | 3 | CIU |
| | | 2 | **QSPI** |
| | | 1 | reference clock |

To give a pad to a peripheral: clear its GPIO enable bit and set the group
bit. To use it as GPIO: set the GPIO enable bit (the group bit may stay).

Other SYSCFG registers named in the tree: GCR1 0x04 (Ethernet mode, section
6.2), GCR2 0x08, CHIP_ID 0x1C, CHIP_REV 0x20, USB OTG control 0x5C, USB1.1
control 0x60, DRAM control 0x6C / 0x70, MEM_CFG 0x74.

### 8.3 What the HT802 selects

From the GPL U-Boot board code (older HT802 hardware revision):

| Function | Pads | Extra |
|---|---|---|
| Console UART | UART1: DGPIO20/21; UART2: EGPIO16/17 or AGPIO28/29 | by strap |
| QSPI | AGPIO7 CLK, AGPIO8..11 D0..D3, AGPIO12 CS0, AGPIO13 CS1; pulls off on CLK and data; NAND group disabled, QSPI group enabled | |
| SPI2 (SLIC) | BGPIO13 DO (pull-up), BGPIO14 DI (pull-down), BGPIO16 CK (pull-up); SPI2 group | chip select is a GPIO |
| TDM1 (PCM) | BGPIO0 clock, BGPIO1 frame sync, BGPIO2 TX, BGPIO3 RX; TDM1 group | |
| Ethernet RMII | EGPIO0..5, 7, 8, 9 to the MAC; EGPIO6 GPIO input; EGPIO11/12 GPIO outputs low; EGPIO10 PHY reset; emaca group | |
| Main clock out | BGPIO31 to peripheral function + GCR1 bit 1 | |
| D-class | BGPIO27 to peripheral function + D-class group | |
| SLIC (old board) | AGPIO28 reset, DGPIO16 chip select, AGPIO17 interrupt | superseded, see below |

From this unit's DTB (V2.5A; these are the ones to use):

| Function | Pad | Global GPIO |
|---|---|---|
| LED WAN / RJ45 / PHONE1 / PHONE2 | EGPIO24 / 25 / 26 / 27 | 152 / 153 / 154 / 155 |
| Reset button | EGPIO22 | 150 |
| SLIC 0 reset / SLIC 1 reset | EGPIO21 / EGPIO19 | 149 / 147 |
| SPI2 chip selects | DGPIO16, DGPIO17 | 112, 113 |
| SLIC interrupts | AGPIO16 (EXTINT0), AGPIO17 (EXTINT1) | 16, 17 |

(The numbers 16 to 27 quoted in `HARDWARE.md` are pin numbers within the
bank, not global GPIO numbers.)

GPIO polarity for LEDs, button and resets is not expressed in the dts (flags
cell is 0 everywhere) and the Grandstream LED/reset drivers were not analysed.

---

## 9. Things a new kernel must get right early

### 9.1 Entry state: MMU and caches may be ON

See 5.5. Booted through `gsboot` the kernel is entered with the MMU on (1:1
sections), D-cache on, I-cache on. Booted with a manual `bootm` from the
prompt it is entered with the MMU off and I-cache on.

- The stock kernel is a zImage, and its decompressor is entered in that state
  on every normal boot, so that path is proven for 3.4.20.
- [infer] The same should hold for a mainline zImage: the decompressor builds
  its own 1:1 table, reloads the translation base, flushes the TLB, and later
  cleans and disables the caches before jumping to the kernel proper. The
  mapping does not change (1:1 before and after), so the D-cache content stays
  valid across the switch. I did not test this.
- A raw `Image` (or any payload whose first instructions assume MMU off) must
  not be started through `gsboot` without a stub that cleans the D-cache and
  turns off MMU and caches first.
- U-Boot cleans the D-cache after copying the kernel to 0xC8008000, but the
  DTB is patched and relocated afterwards, so the DTB contents can still be
  sitting in the D-cache at entry. Harmless as long as the first reader runs
  with the same cache enabled (the decompressor does).

### 9.2 Watchdog

- Registers (0x05700000, 16-bit accesses): CTRL 0x00 (bits 1:0 period: 0 = 1 s,
  1 = 10 s, 2 = 100 s, 3 = 1000 s; bit 2 interrupt enable), ENABLE 0x04 (write
  0xABCD twice to start), REFRESH 0x08 (write 0xA5A5 then 0x5A5A), INTCLR 0x1C,
  INTSTAT 0x20. Interrupt = PLICU source 62. **Once started it cannot be
  stopped.** A watchdog reset is reported in CMU RSTSR bit 3.
- It is **not running at kernel entry** as far as the sources and binaries
  show: the U-Boot tree has no watchdog code at all, the stock kernel config
  has CONFIG_WATCHDOG off, and neither this unit's U-Boot, kernel nor
  Bootastic image contains a watchdog driver string. So a new kernel need not
  service it. (Bootastic source is not available, hence "as far as shown".)

### 9.3 CPU, caches, errata

- ARM926EJ-S, main ID 0x41069265 (r0p5), ARMv5TEJ, VIVT I and D caches,
  32-byte cache lines (L1 cache shift 5), write buffer present, "write buffer
  coherency: ok" in the log.
- Vendor kernel CPU options are the stock ARM926 set: v5TJ abort model, legacy
  prefetch abort, V4WB copy-page, V4WBI TLB, domains, Thumb enabled, D-cache
  in write-back mode, no cache options disabled.
- **No ARM errata options, no vendor patches** in `arch/arm/mm/proc-arm926.S`,
  `arch/arm/kernel/head*.S` or `arch/arm/boot/compressed/head.S` (no DSPG,
  DVF or Grandstream strings in them). Nothing SoC-specific to carry.
- Idle: the deepest state the HT build can reach is plain "wait for interrupt"
  with the I-cache disabled around it, i.e. what mainline's ARM926 idle does.
  The deeper states manipulate the DDR controller and PLL2 and do not apply
  to an SDRAM board.
- FIQ: every PLICU source can be an FIQ; nothing uses it in the HT build.

### 9.4 Bus coherency for DMA masters ("CCU" and the wmb override)

This is the one real SoC quirk in the tree.

- **CCU0 at 0x07000000** is a "coherency control unit": registers CONT 0x00
  (bits from 18 = FIFO depth - 1, bit 17 = "N writes" mode, bit 16 = software
  requests enabled), STAT 0x04 (bits 31:30 state, bit 29 error, bit 16
  software request pending), REQ 0x08, SWTRIG 0x0C (write 0x90EE to start a
  coherency transaction), DUMADR 0x10 (address of a 64-byte-aligned scratch
  area, 16 words), DUMDAT 0x14 (dummy data), RDAT 0x18. Interrupt sources 5
  (CCU0 error) and 1 (CCU1 error).
- Purpose as described by the vendor comments: the memory controller has
  per-port write FIFOs, so data written by one bus master may not yet be
  visible to another. A "CCU barrier" (wait for STAT bit 16 clear, write the
  magic to SWTRIG, wait for bit 16 clear again) pushes dummy writes through to
  guarantee that earlier writes of *other* masters have landed. The vendor
  DMA, NAND, USB, crypto and SD/MMC drivers call it in their completion paths
  before the CPU reads data a device wrote by DMA. The Ethernet driver does
  not call it.
- **`wmb()` and `mb()` are overridden** for this SoC to additionally write a
  word to a coherent-DMA scratch location and read it back, to "make sure the
  CPU port on the Denali controller is flushed" before a device is told to
  look at memory. [src: K `mach/barriers.h`, `arch.c` `dvf_denali_sync`]
  The comment names the Denali DDR controller, which the DVF9918 SDRAM board
  does not use (it uses the MPMC), but the code is unconditional.
- U-Boot enables CCU0 and leaves DUMADR pointing at a variable inside U-Boot's
  relocated image (top of RAM). If the CCU is used it must be re-pointed at
  memory the kernel owns; if it is never triggered the stale pointer is inert
  as far as the code shows. [src: U `ccu_init`]
- Relevance for a first mainline kernel: only bus-mastering devices matter, and
  the only one needed is the GMAC. Mainline ARMv5 `wmb()` compiles to nothing
  that touches the bus. Whether GMAC descriptor/buffer coherency actually
  needs the vendor flush on the MPMC path is not answered by the source (open
  question 6). Symptoms to look for would be sporadic TX stalls or corrupted
  first words of received frames.

### 9.5 Things that are fine to ignore

- CMU "SEC mutex": only matters while the CSS core runs.
- CSS core: held in reset by default (reset bits 1 and 23 are among those the
  kernel does not release); gates 1 and 23 off after the vendor clock init.
  Leave it alone. Note TDM1 is enabled by U-Boot and the stock dts hands TDM
  to the CSS (`tdm-ownership = 3`); an own TDM driver should reinitialise the
  block.
- The vendor kernel's software reset: write 1 to RSTSR bits 3, 4, 5, then
  0x7496398B and 0xB5C9C674 to SWSYSRST (CMU 0x40). Useful as the restart
  handler.

### 9.6 Kernel address-space note

Physical RAM starts at 0xC8000000, above the usual kernel virtual base. The
vendor kernel uses PAGE_OFFSET 0xD0000000 for exactly this configuration
(the Kconfig default changes when "boot from SDRAM" is selected), with
PHYS_VIRT patching enabled and zreladdr 0xC8008000. Lowmem is then
0xD0000000..0xD1FFFFFF and vmalloc starts at 0xD2800000. I found no comment
explaining the choice; [infer] it avoids having the kernel's own 1:1 "turn the
MMU on" mapping of 0xC80xxxxx land inside the kernel virtual area. With the
default 0xC0000000 the 1:1 section would sit at 0xC8000000, above a 32 MiB
lowmem (ends 0xC2000000), so it may work, but it is the one place where the
vendor deliberately deviated and worth testing first if early boot dies
silently.

---

## 10. Open questions

1. **Is the shipped U-Boot console locked?** Shipped default environment has
   `console=no` and the binary contains `gxp_console=1`; the GPL HT build has
   neither and never locks. If the lock is active, no prompt and no output
   until the environment says `console=yes`. The environment partition is
   erased, so the default applies. Needs the UART to answer. If locked, the
   GPL logic suggests a saved environment with `console=yes` (8 KiB at
   0xFBE000, standard U-Boot CRC32 + data format) would unlock it; unverified
   for the shipped build and it is a flash write.
2. **Bootastic's rule for choosing uboot vs uboot2**, and whether it also
   leaves anything running (watchdog, timers). No source. On this unit the
   second copy (0xFC0000) has the higher provision counter.
3. **Reset values** of SWCHWRST1/2, SWCLKENR1/2 and of the PLICU priority and
   priority-mask registers. The vendor code never relies on them (it releases
   resets wholesale and never writes priorities), so a new kernel should
   release the resets it needs explicitly.
4. **Main oscillator strap**: 25 or 50 MHz (RSTSR bit 2) on this board.
   U-Boot assumes 25 MHz when programming PLL1; the V1.x log (400 MHz, PCLK
   133) is consistent with 25. Read CMU 0x00, 0x14, 0x1C, 0x48 on the box to
   pin down hclk exactly (`/dev/mem` or `/proc/clocks` on the stock kernel).
5. **V2.5A Ethernet details**: PHY MDIO address, reset GPIO, whether the SoC
   or a crystal supplies the 50 MHz reference, and the GCR1 mode field
   (RMII = 4 expected). The GPL tree only knows the KSZ8081 board (address 0,
   reset EGPIO10). The `eth_interface=RGMII` reading in `HARDWARE.md`
   contradicts the 10/100 YT8512H and the dts; SYSCFG 0x05200004 on the
   running unit settles it.
6. **Does the GMAC need the vendor "flush the memory controller" step** on
   the SDRAM/MPMC path (section 9.4)? Not decidable from source.
7. **SP804 compatibility of the timer block** beyond the four registers the
   vendor uses (RIS/MIS/BGLoad, ID registers).
8. **PLICU STATUS and PENDING_REQ semantics** and whether CAUSE is "pending
   and enabled" filtered by priority. The driver only needs CAUSE.
9. **QSPI in single-line mode**: all vendor use is QPI. Field encodings for
   single-line read/program are in the tree (the Atmel/Winbond entries use
   single-line opcode and address with quad data) but untested on this chip.
10. **The Grandstream header fields at 0x18, 0x1C, 0x24, 0x54** and whether
    the Linux-side updater checks more than U-Boot does (U-Boot checks only
    magic and checksum).
11. **GPIO polarities** for LEDs, reset button and SLIC resets on V2.5A.
12. **Mainline zImage entered with MMU on** (section 9.1): reasoned to work,
    not tested.
