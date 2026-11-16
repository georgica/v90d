# DVF9918 I/O facts for from-scratch drivers (SPI, TDM, watchdog, GPIO)

Hardware facts only, restated in my own words from the Grandstream HT8xx GPL
tarball. No vendor code is reproduced. Complements `HARDWARE.md`; nothing from
there is repeated except where it has to be corrected.

Sources and how they are cited:

- `K:` = kernel tree `linux-3.4.20/` (the GPL 3.4.20 tree).
- `U:` = the U-Boot tree shipped in the same tarball (`uboot/`). It was read
  only because the kernel never programs the pad mux, and U-Boot does.
- `live dts` = `v90d/recon/backup/board.dts` (this unit).
- "GPL dts" = `K:devtree/ht802_9610003713b.dts` (newest HT802 board in the tree).

Caveat that applies to everything below: the unit runs a kernel build newer
than the GPL tree (the 9610003725A board is not in the tree, and HARDWARE.md
reports `spidev` built in while `K:defconfig_ht80x:966` has it off). Register
facts are a property of the silicon and should hold. Statements about "which
driver is built / bound" are from the GPL defconfig and must be confirmed on
the unit where marked.

## 0. Corrections to HARDWARE.md and shared infrastructure

### 0.1 GPIO numbers in HARDWARE.md are pin-within-bank, not global numbers

The GPIO specifier has three cells: bank, pin, flags
(`K:drivers/gpio/gpio-dspg.c:319` `dspg_gpiolib_xlate`, `:594`). So, from the
live dts:

| Signal | dts cells | Name | Linux GPIO no. (bank*32+pin) | Bank register base |
|---|---|---|---|---|
| SPI CS, SLIC 0 | 3 16 | DGPIO16 | 112 | 0x05000120 |
| SPI CS, SLIC 1 | 3 17 | DGPIO17 | 113 | 0x05000120 |
| SLIC 0 reset | 4 21 | EGPIO21 | 149 | 0x05000180 |
| SLIC 1 reset | 4 19 | EGPIO19 | 147 | 0x05000180 |
| SLIC 0 INT | 0 16 | AGPIO16 (EXTINT0) | 16 | 0x05000000 |
| SLIC 1 INT | 0 17 | AGPIO17 (EXTINT1) | 17 | 0x05000000 |
| LED_WAN / RJ45 / PHONE1 / PHONE2 | 4 24..27 | EGPIO24..27 | 152..155 | 0x05000180 |
| Reset button | 4 22 | EGPIO22 | 150 | 0x05000180 |

"GPIO 16 and 17" for chip select are therefore not the same pins as the
"GPIO lines 16/17" of the SLIC interrupt: CS is on bank D, INT on bank A.

### 0.2 Hardware interrupt numbers

The numbers in HARDWARE.md (41, 43, ...) are Linux virtual IRQs. The PLIC
hardware IDs are: SPI2 = 44, SPI1 = 45, TDM1 = 60, TDM2 = 59, watchdog = 62,
GDMAC done = 21, GDMAC error = 20, EXTINT0..6 = 24..30, EXTINT7..15 = 32..40
(`K:arch/arm/mach-dvf/include/mach/irqs.h:51-95`, live dts).

### 0.3 Naming trap: block numbering is 1-based in the SoC, 0-based in the dts

"TDM0" at 0x05F00000 is `tdm1` / `DVF_TDM1_BASE` / `RESET_TDM1` / clock `tdm1`
in the kernel. The SLIC SPI at 0x05E00000 is `spi2` (`DVF_SPI2_BASE`), a
different controller type ("dcx") from `spi1` at 0x05D00000 ("dmw", disabled).
(`K:arch/arm/mach-dvf/include/mach/hardware.h:133-136`.)

### 0.4 Clock management unit (CMU), base 0x05300000

All 32-bit registers. (`K:arch/arm/mach-dvf/include/mach/hardware.h:177-197`,
`K:arch/arm/mach-dvf/clock.c`.)

| Offset | Name | Fields relevant here |
|---|---|---|
| 0x00 / 0x04 / 0x08 | PLL1 / PLL2 / PLL3 control | [4:0] pre-divider minus 1; [11:5] multiplier minus 1; [13:12] post-divider as a right shift (0..3); [14] VCO band, set when VCO > 600 MHz; [15] power-down (1 = PLL off); [16] bypass (U-Boot only); [18] lock flag; PLL1 [20:19] input select (0 = main osc, 1 = 13.824 MHz, 2 = 12 MHz, per U-Boot). `clock.c:967` `get_rate_pll`, `:1042-1046`, `:1083`, `:1415`; `U:arch/arm/cpu/arm926ejs/dvf99/clock.c:195-252` |
| 0x14 | CPUCLKCNTRL | [3:0] CPU divider minus 1 (from sysclk); [5:4] AHB divider code (value+1, 0 means 3); [15:14] altsysclk source; [17:16] audsrcclk source (0 = mainclk, 1 = 13.824 MHz DECT osc, 2 = 12 MHz osc, 3 = mainclk). `clock.c:136-148`, `:213-223`, `:1110` |
| 0x1C | CLKSWCNTRL | [1] sysclk source (0 = altsysclk, 1 = PLL1); [2] audclk source (0 = audsrcclk, 1 = PLL3); [20:19] TDM1 clock source; [22:21] TDM2 clock source. TDM source codes: 0 = sysclk, 1 = audclk, 2 = PLL2, 3 = sysclk. `clock.c:193-203`, `:227-236`, `:357`, `:377-380` |
| 0x28 | CLKDIVCNTRL3 | [31:24] SPI2 divider minus 1 (8 bits), input sysclk. `clock.c:307` |
| 0x2C | CLKDIVCNTRL4 | [11:0] TDM1 divider minus 1 (12 bits); [23:12] TDM2 divider minus 1. `clock.c:377-380` |
| 0x48 | RSTSR | [1] mainclk source; [2] main oscillator is 25 MHz when set, 50 MHz when clear; [3] "last reset was a watchdog reset" flag; [4],[5] other reset indications, all three written as 1 before a software reset. `clock.c:858`, `:1725`; `K:arch/arm/mach-dvf/arch.c:163-174` |
| 0x40 | SWSYSRST | Software system reset: write 0x7496398B then 0xB5C9C674. `arch.c:172-173` |
| 0x50 | SWCHWRST1 | Block resets 0..31, 1 = held in reset. bit 0 GDMAC, bit 9 TDM1, bit 10 TDM2. `K:arch/arm/mach-dvf/include/mach/reset.h`, `clock.c:1683` `reset_set_onoff` |
| 0x54 | SWCHWRST2 | Block resets 32..63. bit 6 SPI1, bit 7 SPI2. same |
| 0x58 | SWCLKENR1 | Clock gates 0..31, 1 = running. bit 0 GDMAC, bit 9 TDM1, bit 10 TDM2. `clock.c:894-930` |
| 0x5C | SWCLKENR2 | Clock gates 32..63. bit 6 SPI1 (gate 38), bit 7 SPI2 (gate 39). same |

Generic divider rule: output = parent / (field + 1) (`clock.c:1166`
`get_rate_div`). The kernel's set-rate refuses any rate that is not an exact
integer division of the parent as it computes it (`clock.c:1175`), and its
round-rate picks the nearest integer divider (`clock.c:1200`).

Traps:

- The reset and gate registers (0x18, 0x50..0x5C) are marked "guarded by SEC
  CMU mutex" in `hardware.h:181-195`: the CSS core writes them too. The ARM
  kernel does not take any mutex; it does read-modify-write with IRQ and FIQ
  off and repeats until the read-back matches (`clock.c:906-925`,
  `:1695-1707`). A user-space read-modify-write on these registers can lose a
  race against both the kernel and the CSS. Use the same write-and-verify loop,
  or do it from a kernel module through `clk_*` / `reset_set` / `reset_release`
  (exported, `clock.c:1715`, `:1722`).
- At boot the kernel releases almost all block resets, including TDM1 and
  SPI2 (masks 0x0F7BFFDD and 0x003F0FF7 cleared, `clock.c:1926-1932`), and
  then gates every clock that is running but has no kernel user
  (`clock.c:1657` `clk_init_node`). So whatever U-Boot enabled is gated again
  until a driver claims it.
- The kernel clock layer keeps reference counts in RAM. Flipping a gate bit
  from /dev/mem leaves those counts wrong; the next kernel enable/disable may
  then do nothing or do the opposite of what is expected.

### 0.5 System configuration (pad enables), base 0x05200000

SPCR1 at offset 0x54, SPCR2 at 0x58, one bit per pad group, 1 = pads handed to
the peripheral (`hardware.h:207-260`, `U:arch/arm/cpu/arm926ejs/dvf99/pads.c`).
Relevant SPCR1 bits: 23 = TDM1 pads, 22 = TDM2 pads, 20 = SPI1 pads,
19 = SPI2 pads, 26 / 25 = UART1 / UART2 pads.

The kernel never writes SPCR1. It only sets SPCR2 bit 17 when CGPIO31 is
requested (`gpio-dspg.c:197-203`). The pad mux for SPI2 and TDM1 is done once
by U-Boot (sections 1.6 and 2.8).

---

## 1. SPI controller ("dcx"), base 0x05E00000, hw IRQ 44

Source: `K:drivers/spi/spi-dspg-dcx.c` unless noted. The header
`K:include/linux/spi/spi_dspg_dcx.h` contains only a bus-number field, no
hardware information.

### 1.1 Registers (all accessed as 16-bit)

| Offset | Name | Use |
|---|---|---|
| 0x00 | GLOBAL | bit 1 = block reset, bit 2 = FIFO pointer reset. Both self-clearing: the driver writes and then spins until the register reads 0 (`:44`, `:53-54`, `dspg_dcx_spi_reset` `:248`, probe `:480`) |
| 0x04 | CON | Control, see 1.2 |
| 0x08 | FRM | Frame (word) count for the burst. Driver writes N when N > 1 and 0 when N = 1 (`dspg_dcx_spi_set_frame_count` `:205`). Reads back as the number of words still outstanding (used that way in the IRQ handler, `:331`) |
| 0x0C | IER | Interrupt enables: bit 7 INTCS, bit 6 end-of-transfer, bit 4 RX threshold, bit 1 TX threshold (`:63-66`). Only bits 6 and 4 are ever used |
| 0x10 | STAT | bit 6 busy, bit 5 RX FIFO full, bit 4 RX at threshold, bit 3 RX empty, bit 2 TX full, bit 1 TX at threshold, bit 0 TX empty (`:69-75`). Only bit 6 is read by the driver |
| 0x14 | DAT | FIFO data, one word per access. A read also starts a receive burst (1.4) |

There is no interrupt status/clear register in the driver; the interrupt line
is requested as rising-edge (`:476`) and the handler simply writes IER = 0.

### 1.2 CON bits

| Bits | Meaning | Evidence |
|---|---|---|
| 15 | RX threshold select (unused) | `:57` |
| 14 | TX threshold select (unused) | `:58` |
| 13 | "shift off": used only for a single-word receive, set after the dummy read | `:59`, `:289-296` |
| 12:9 | word length minus 1 (driver accepts 3..16 bits) | `:274`, `:148` |
| 8 | SPI mode bit: set for mode 3, clear for mode 0 | `:60`, `:275` |
| 7 | master | `:61` |
| 0 | always written as 1; meaning not stated in the source (see open questions) | `:274` |

Value used for the SLIC (mode 3, 8 bit): 0x0080 | (7 << 9) | 0x0100 | 1 = 0x0F81.

### 1.3 Limits and mode

- FIFO depth 32 words, software RX threshold 8 (`:77-78`).
- Only SPI modes 0 and 3 are accepted (`:153`). The SLIC client sets mode 3,
  8 bits (`K:drivers/gs/gs-slic.c:112-113`), 1 MHz max (dts).
- Effectively half duplex. If a transfer has a TX buffer the driver takes the
  transmit path and never stores received words; the receive path is taken
  only when there is no TX buffer (`dspg_dcx_spi_txrx` `:277-303`). What MOSI
  carries during a receive burst is not stated.

### 1.4 How a transfer is done (interrupt driven, no polling, no DMA)

Before every transfer: FIFO pointer reset through GLOBAL, wait for it to read
0, 1 us pause, write CON (`:248-256`).

Transmit of N words (N <= 32 per burst):
1. FRM = N (0 if N = 1).
2. IER = end-of-transfer only.
3. Write N words to DAT. Shifting starts by itself.
4. On the end-of-transfer interrupt: IER = 0. If more data remains, wait 1 us,
   redo the FIFO reset + CON write, and send the next chunk of up to 32
   (`:367-380`). Otherwise the transfer is complete.

Receive of N words:
- N = 1: with interrupts masked, IER = end-of-transfer | RX threshold, do one
  dummy read of DAT, then rewrite CON with bit 13 set (`:285-296`).
- N > 1: FRM = N, IER = end-of-transfer | RX threshold, one dummy read of DAT
  starts the clocking (`:297-302`).
- In the interrupt: IER = 0; words available = remaining minus FRM if STAT
  busy is set, else all remaining; read that many from DAT; loop while data
  remains and (at least 8 are available or FRM < 32); otherwise re-arm IER and
  wait for the next interrupt (`:326-360`).

A polled driver can do the same with IER left at 0 and STAT bit 6 (busy) plus
the FIFO flags as the completion test. That is an inference: the vendor driver
never polls.

### 1.5 Clock, divider, enables

- Functional clock `spi2`: sysclk / (CLKDIVCNTRL3[31:24] + 1), gate
  SWCLKENR2 bit 7, reset SWCHWRST2 bit 7 (0.4; `clock.c:307`, `reset.h`).
- The controller halves its input clock: SCK = spi2 clock / 2. The driver
  therefore asks the clock layer for twice the wanted bit rate (`:158-160`).
  SCK = sysclk / (2 * (div + 1)). There is no divider inside the SPI block
  that the driver programs.
- For 1 MHz SCK the spi2 clock is set to 2 MHz. With an 8-bit divider the
  lowest reachable spi2 clock is sysclk / 256.
- sysclk rate: not a constant in the tree. It is PLL1 (or altsysclk) as left
  by the boot loader; the kernel reads the PLL register to compute it. See
  open questions.
- Probe order (`:377-490`): get clock, request IRQ, set rising-edge trigger,
  enable clock, GLOBAL = block reset | FIFO reset, wait for 0, 100 us. The
  clock is then left on permanently (the per-transfer gating is commented out,
  `:271`, `:305`).
- U-Boot does the same bring-up earlier: release reset "spi2", enable clock,
  set 2 MHz (`U:board/dspg/evb9918/spi.c:31-66`).

### 1.6 Pins and chip select

- Pads (U-Boot, `spi.c:35-40`, `:63`): BGPIO13 = SPI2 data out, BGPIO14 = SPI2
  data in, BGPIO16 = SPI2 clock. Each is taken out of GPIO mode (bank B enable
  bit cleared) with pull enabled (up on DO and CK, down on DI), then SPCR1
  bit 19 is set.
- Chip select is a plain GPIO, active low: 0 = selected, 1 = idle
  (`dspg_dcx_spi_chipsel` `:106-118`). With `CONFIG_HT` the driver reads only
  entry 0 of the `current_cs_gpio` property, requests it as "fxs-cs" and
  drives it high (`:398-402`). That is DGPIO16 (Linux 112).
- The GPL driver therefore toggles the same single CS GPIO for every SPI
  device on the bus. The second entry in the live dts (DGPIO17) is not used
  by any code in the GPL tree. See open questions.
- SLIC reset lines are only requested, not pulsed, by the open code
  (`gs-slic.c:123-130`); the closed SLIC module does the reset. A helper in
  the same file shows the intended pulse: high, 100 us, low for 1 ms, high,
  1 ms (`gs-slic.c:54-62`).

---

## 2. TDM block (TDM1 = dts "tdm@05f00000"), base 0x05F00000, size 0x2000, hw IRQ 60

Sources: `K:drivers/gs/gs-tdm.c` (the driver used on the HT802) and
`K:sound/soc/dvf/dspg-tdm.c` (the ALSA original it was cut down from; same
register map). All registers 32-bit.

### 2.1 Registers

| Offset | Name | Fields |
|---|---|---|
| 0x0000 | ENABLE | bit 0: 1 = TDM machine running |
| 0x0004 | CONFIG_1 | [4:0] slots per frame minus 1; bit 8 TX edge (1 = drive TX data on falling bit clock, 0 = rising); bit 9 RX edge (see open questions for polarity); bit 10 frame-sync edge (0 = FSYNC in phase with the bit clock, used for the telephony format); bit 11 FSYNC direction (1 = SoC generates FSYNC); bit 12 bit-clock direction (1 = SoC generates the bit clock); bit 13 auto-resync on FSYNC (used only when FSYNC is an input) |
| 0x0008 | CONFIG_2 | [2:0] FIFO "bank size" code: 0 = 2 FIFO lines of 64 entries (2-slot frames), 1 = 4 lines of 32 entries (4-slot frames), other codes unknown; bit 8 "truncate/align" for slot widths below 20 bits; bit 9 shift direction, 1 = MSB first |
| 0x0010 | FSYNC | Position of the FSYNC rising edge: [15:8] bit index within the slot, [4:0] slot index. 0 = first bit of slot 0 |
| 0x0014 | FSYNC_DUR | FSYNC pulse length in bit clocks. The HT driver writes 0, the ALSA driver writes (slot width minus 1). Exact encoding (value or value+1) not stated |
| 0x0018 | FIFO_WM | [5:0] TX watermark (emptiness level), [13:8] RX watermark (fullness level), unit = frames. The trigger point is field value + 1 |
| 0x0020 | INT_EN | bit 0 TX watermark, bit 1 TX empty, bit 2 RX watermark, bit 3 RX full |
| 0x0028 | INT_CLR | write 1 to clear: bit 0 TX underrun, bit 1 RX overrun, bit 2 FSYNC, bit 3 TX flags. "Clear all" is written as 0xFFFFF |
| 0x002C | INT_STATUS | bit 1 TX underrun, bit 3 RX overrun (other bits not named) |
| 0x0030 | STATUS | defined, never used; bits unknown |
| 0x0038 | TX_FIFO | write one word per slot, in slot order |
| 0x003C | RX_FIFO | read one word per slot, in slot order |
| 0x0A00 | TX_REG_0 | defined, never used (presumably direct view of FIFO storage) |
| 0x1200 | RX_REG_0 | defined, never used |
| 0x1800 + 4*i | FDA entry i | Frame descriptor for slot i: [15:8] slot index, [7:6] written as 3 (meaning not stated), bit 5 "control/status" slot type (0 = audio), [4:0] slot width in bit clocks |

(`gs-tdm.c:117-157`, `:195-196`.)

Hardware properties stated in driver comments:

- FIFO words are 20 bits wide and are the 20 most significant bits of the
  32-bit value read or written (`gs-tdm.c:159-163`).
- Slot width must be 8..20 bit clocks (`:396-399`).
- TX and RX FIFOs hold 128 samples each, described as 64 frames of 2 slots
  (`:165-169`). With 4 slots per frame that is 32 frames, which is why the
  driver halves the watermark for 4 slots (`:588-591`).
- As FSYNC master the block can only produce a rising (active-high) FSYNC
  pulse; as slave it only synchronises on a rising edge (`:475-496`).
- The watermark interrupts are level-type and are asserted even while ENABLE
  is 0, so the FIFOs must be pre-filled / drained before unmasking
  (`:605-614`).
- The block misbehaves (wrong FSYNC alignment, "metallic sound") if its clock
  is running while it is being configured. Workaround used everywhere:
  configure with the clock gated (`:437-450`).
- There is no companding option anywhere in the register map as used. Samples
  are raw linear words of the configured slot width.

### 2.2 What the HT802 configuration is

Fixed in `dspg_tdm_dev_probe` (`gs-tdm.c:777-790`), not taken from the dts:

| Parameter | Value |
|---|---|
| Frame rate | 8000 Hz |
| Slots per frame | 4 |
| Bits per slot | 16 (linear) |
| Bit clock | 8000 * 16 * 4 = 512 kHz, generated by the SoC |
| FSYNC | generated by the SoC, active-high pulse rising on bit 0 of slot 0, in phase with the bit clock, duration field 0 |
| Data | MSB first, TX changes on the rising bit clock edge |
| Ownership | flagged to be handed to the CSS |

Resulting register values (computed from the code, `dvf_tdm_configure`
`:371-635`):

| Register | Value | Why |
|---|---|---|
| FDA 0..3 (0x1800..0x180C) | 0x00D0, 0x01D0, 0x02D0, 0x03D0 | index << 8, 0xC0, width 16 |
| CONFIG_1 | 0x1A03 | 3 = four slots; bit 9 set; bit 8 and bit 10 cleared by the telephony format; bits 11 and 12 = master |
| CONFIG_2 | 0x0301 | MSB first, align, 4-line FIFO |
| FSYNC | 0 | |
| FSYNC_DUR | 0 | |
| FIFO_WM | 0x1F1F | CSS level 62, halved for 4 slots = 31, both directions |

For an ARM-owned (non-CSS) setup the same code would use watermark 16, halved
to 8 for 4 slots: FIFO_WM = 0x0808.

Slot-to-channel mapping: slot i of the frame is FIFO word i of each group of
four, in both directions. Which SLIC channel talks in which slot is programmed
into the SLIC (its PCM TX/RX start registers) by the closed SLIC module, not by
anything in this tree. See open questions.

### 2.3 Exact init sequence performed by the driver

`dspg_tdm_dev_probe` (`gs-tdm.c:692-806`) then `dvf_tdm_configure`:

1. Read `id` and `reset` (= 9) from the dts, get clock (device name
   `dvf-tdm.0` maps to clock `tdm1`, `K:arch/arm/mach-dvf/arch.c:152`,
   `clock.c:792`), claim 0x05F00000..+0x2000, request the IRQ.
2. Ask the clock layer for the nearest achievable rate to 512 kHz and set it
   (only done when both clock and FSYNC are master, `:415-428`).
3. Reset (`dvf_tdm_reset` `:271-300`): ENABLE = 0; gate the TDM clock; wait
   20 us; assert block reset (SWCHWRST1 bit 9); wait 2 us; release reset.
   The clock stays gated.
4. Write FDA entries, CONFIG_1, CONFIG_2, FSYNC, FSYNC_DUR, FIFO_WM.
5. Ungate the TDM clock (SWCLKENR1 bit 9).
6. Pre-fill and drain: write (wm + 1) * slots zeros to TX_FIFO and do the same
   number of reads from RX_FIFO (128 each with the HT values, `:615-628`).
7. ENABLE = 1.
8. Create `/proc/gs/css_own_tdm`.

INT_EN is not written during init (stays at its reset value).

### 2.4 Hand-over to the CSS (what actually runs on the HT802)

Writing a string starting with `1` to `/proc/gs/css_own_tdm` calls the grant
path, anything else calls the revoke path (`prov_tdm_write_proc` `:676-690`).

Grant (`css_tdm_grant` `:325-368`):
1. ENABLE = 0.
2. Disable the TDM interrupt at the ARM interrupt controller.
3. INT_EN = 0xF (all four sources), to be serviced by the CSS.
4. Send a "grant" message over the coma service named `tdm`: type 0, TDM id 0,
   cookie 0, channels 4, sample size 16, rate 8000, each a 32-bit word; wait
   up to 1 s for an ACK (type 2); a NACK (type 3) is treated as fatal
   (`K:drivers/staging/dspg/coma/coma-tdm.c:35-67`, `:100-125`, `:168-185`).
   The ARM does not set ENABLE again; the CSS is expected to.

Revoke (`css_tdm_revoke` `:302-323`): send type 1 with the id, then ENABLE = 1.
The ARM interrupt is not re-enabled and INT_EN is not cleared.

According to a comment the CSS services the FIFOs from its own interrupt and
moves 4 frames per watermark interrupt (`gs-tdm.c:198-203`).

### 2.5 Which driver is used on the HT802, and who moves samples

- `K:defconfig_ht80x`: `CONFIG_SOUND` off (line 1079), so nothing in
  `sound/soc/dvf` is built, and `GS_TDM` / `GS_SLIC` depend on `!SOUND`
  (`K:drivers/gs/Kconfig`). `CONFIG_GS_TDM=y`, `CONFIG_GS_SLIC=y` (1158-1159),
  `CONFIG_DSPG_CSS_TDM=y` (1127), `CONFIG_DSPG_CSS_VOICE=y`,
  `CONFIG_DMW96_GDMAC` off (1132), `CONFIG_DMADEVICES` off.
- So the bound driver is `gs-tdm.c` (it registers under the name `dvf-tdm`
  and names its IRQ `dvf_tdm_irq`, matching what the unit shows). The
  `snd-*` / `dvf99_css_codec` nodes in the dts have no driver.
- `coma-tdm.c` is not a TDM driver. It only carries the grant / revoke
  messages to the CSS.
- dts: `css { tdm-ownership = <3> }` (both TDMs may be owned by the CSS). The
  CSS loader uses the mask to decide whether the CSS may control the TDM
  clocks and resets (`K:drivers/staging/dspg/css.c:1099`, `:1155-1163`).
- Conclusion: in the stock configuration the CSS DSP core moves every sample.
  The ARM only configures the block, pre-fills zeros, and hands it over. The
  ARM interrupt handler only reports underrun / overrun
  (`dvf_tdm_irq` `:637-674`). There is no code path in `gs-tdm.c` that reads
  or writes audio through the FIFO. This matches the single TDM interrupt
  seen on the unit.
- After the grant the audio leaves the CSS as coded RTP through the coma
  voice service (`K:drivers/staging/dspg/coma/voice.h`: codec lists, jitter
  buffer, T.38), not as raw PCM. The tree has no interface that gives the ARM
  raw samples while the CSS owns the TDM.

### 2.6 DMA

- The ALSA variant uses the on-chip DMA controller for ARM-side transfer, not
  interrupts: channel 8 = TDM1 RX (peripheral address base + 0x3C), channel
  9 = TDM1 TX (base + 0x38), 32-bit width, hardware-requested single mode,
  burst length 32 words ("16 stereo frames"), priorities 0 and 1
  (`K:arch/arm/mach-dvf/fixups.c:92-110`, `:154-158`). The FIFO watermark
  doubles as the DMA request level and must be at least the burst size in
  frames (`gs-tdm.c:171-190`).
- DMA controller: base 0x06400000, size 0x200, clock gate SWCLKENR1 bit 0,
  reset SWCHWRST1 bit 0, hw IRQs 21 / 20 (`fixups.c:183-197`). It also needs
  256 bytes at the top of the on-chip SRAM (0x00207F00) for descriptors
  (`hardware.h:154-157`).
- On the HT802 the platform device `dmw96dma` is always registered
  (`fixups.c:245`), which is why it shows up in `/proc/iomem`, but its driver
  is not built in the GPL config. The PCM glue the ALSA driver refers to
  ("dvf-pcm.c") is not in the tree. The DMA register map itself is in
  `K:drivers/staging/dspg/dmw96dma.c` and was not extracted for this note.

### 2.7 What a minimal ARM-side driver must do (derived, not a vendor path)

No programmed-I/O sample path exists in the tree, so this is assembled from
the facts above and needs verification on hardware.

1. Make sure the CSS does not own the block (revoke, or CSS not running).
2. Sequence 2.3 steps 2..7 with your own slot layout and a mid-range
   watermark. Keep the clock gated while writing the configuration.
3. Per frame, write one 32-bit word per slot to TX_FIFO (0x38) and read one
   per slot from RX_FIFO (0x3C), always whole frames, slot 0 first. Unused
   slots still need a word.
4. Sample placement in the 32-bit word: the top 20 bits are used; with a
   16-bit slot and the align bit set, where the 16 bits sit is not stated
   (open question). A loopback of a known pattern will settle it.
5. Pace on the watermark interrupts (INT_EN bits 0 and 2) or on a DMA
   channel; clear underrun / overrun through INT_CLR bits 0 and 1 and
   reconfigure after an xrun (the ALSA driver redoes the whole reset +
   configure, `dspg-tdm.c:796-800`).
6. Budget: the FIFO holds 128 words per direction. With 4 slots that is 32
   frames = 4 ms at 8 kHz; with 2 slots, 64 frames = 8 ms. That is too short
   for a user-space polling loop on this CPU; it needs an interrupt handler
   or DMA in the kernel.

### 2.8 Clock tree and pins for TDM1

- TDM1 clock = source / (CLKDIVCNTRL4[11:0] + 1); source chosen by
  CLKSWCNTRL[20:19] (0 sysclk, 1 audclk, 2 PLL2, 3 sysclk); gate SWCLKENR1
  bit 9; reset SWCHWRST1 bit 9 (0.4).
- When the SoC is bit-clock master the TDM clock rate is the bit clock: the
  driver sets the clock to rate * bits * slots directly (`gs-tdm.c:421-427`).
  FSYNC is derived inside the TDM block from the slot count and widths, so
  frame rate = bit clock / (sum of slot widths).
- Neither the kernel nor U-Boot ever writes the TDM1 source mux; both only
  read it (`clock.c:832`, `U:.../dvf99/clock.c:472-495`). Its value is the
  reset default or whatever the first-stage loader left. Not determinable
  from the tree.
- Because the divider is an integer, 512 kHz is exact only if the source is a
  multiple of 512 kHz. Example: if the source is a 400 MHz sysclk, the
  nearest divider is 781, giving 512 163.9 Hz and an 8002.56 Hz frame rate
  (+320 ppm). The real numbers must be read on the unit (`/proc/clocks`
  prints every clock with rate and parent, `clock.c:1564-1642`, `:1982`).
- The CSS can also change this clock: with the ownership mask set, the CSS
  firmware may ask the ARM kernel to enable, disable, set or query the rate
  of `tdm1`, and to assert or release the TDM1 reset, through SEC messages
  (`css.c:426-484`). Whether the stock firmware changes the rate after the
  grant is not visible in the tree.
- U-Boot's own TDM bring-up (`U:board/dspg/evb9918/tdm.c:31-45`): release
  TDM1 reset; hand BGPIO0 (bit clock), BGPIO1 (FSYNC), BGPIO2 (data TX),
  BGPIO3 (data RX) to the peripheral (bank B enable bits cleared); set SPCR1
  bit 23; set the TDM1 clock to 2.048 MHz; write CONFIG_1 = 0x1801 (clock
  and FSYNC master, 2 slots); ENABLE = 1. So the SLIC already sees a bit
  clock before Linux starts; the kernel later gates it and reconfigures to
  512 kHz.

---

## 3. Watchdog, base 0x05700000, size 0x100, hw IRQ 62

Source: `K:drivers/watchdog/dspg_wdt.c`. Registers accessed as 16-bit.

| Offset | Name | Use |
|---|---|---|
| 0x00 | CTRL | [1:0] period: 0 = 1 s, 1 = 10 s, 2 = 100 s, 3 = 1000 s; bit 2 = interrupt enable |
| 0x04 | ENABLE | start: write 0xABCD twice in a row |
| 0x08 | REFRESH | feed: write 0xA5A5 then 0x5A5A |
| 0x1C | INTCLR | defined, unused |
| 0x20 | INTSTAT | defined, unused |

Behaviour:

- Probe only sets the period to 10 s and registers `/dev/watchdog`
  (misc minor 130). It logs "initialized, not started" (`dspg_wdt_probe`
  `:161-203`).
- The timer starts only when user space opens the device (`dspg_wdt_open`
  `:71-81`). Feeding is by any write or the keepalive ioctl. Once started it
  cannot be stopped; close just feeds once more, after which the unit resets
  about 10 s later (`:83-94` and the comment there).
- No timeout ioctl; the period is fixed at 10 s by the driver.
- The watchdog has no entry in the clock table (no gate).
- A watchdog reset is reported by CMU RSTSR bit 3 (`clock.c:1725`).

Who enables it:

- dts: the node exists in `dvf99.dtsi:115-119` and in the live dts with no
  `status` property, so a driver would bind if present.
- GPL kernel: `CONFIG_WATCHDOG` is off in `defconfig_ht80x:1024` (also in the
  `_nfs` and `ht81x` configs), so the driver is not built and nothing in the
  kernel touches 0x05700000.
- U-Boot: no access to the watchdog anywhere in the DVF99 CPU or board code
  (only a "FIXME: add watchdog here" in `U:.../dvf99/upgrade.c:99`).
- Bootastic (first stage) and the mask ROM: no source, unknown.

Practical answer: per the GPL sources the hardware watchdog is never started,
so killing user-space daemons should not cause a hardware-watchdog reset. This
must be confirmed on the unit because its kernel is a newer build: check for
`/dev/watchdog`, a `dspg_wdt` line in `/proc/misc`, `/proc/interrupts` and
`/proc/iomem` (05700000), and which process holds the device open. A
software supervisor in the stock user space that reboots when a daemon dies is
a separate matter and outside this tree.

---

## 4. GPIO, base 0x05000000, size 0x2B0

Sources: `K:drivers/gpio/gpio-dspg.c`, `K:include/linux/gpio-dspg.h`,
`U:arch/arm/cpu/arm926ejs/dvf99/gpio.c`.

### 4.1 Layout

- 5 banks on this SoC (A..E), 32 pins each, one register block of 0x60 bytes
  per bank: bank base = 0x05000000 + bank * 0x60 (A 0x000, B 0x060, C 0x0C0,
  D 0x120, E 0x180).
- Linux GPIO number N: bank = N >> 5, bit = N & 31. dts cells: bank, pin,
  flags.
- Pins that do not exist (`dspg,nx_masks`): AGPIO14, 15, 20..25, 30, 31;
  BGPIO4..7, 15, 27..31; DGPIO27..29.

### 4.2 Per-bank registers (32-bit, one bit per pin)

Every setting has a read-back register plus separate write-1-to-set and
write-1-to-clear registers, so no read-modify-write is needed.

| Offset | Purpose |
|---|---|
| 0x00 | data, read (pin level; used for both inputs and outputs) |
| 0x04 / 0x08 | output data set / clear |
| 0x0C | direction read-back: 1 = input, 0 = output |
| 0x10 / 0x14 | make output / make input |
| 0x18 | GPIO-enable read-back: 1 = pin is a GPIO, 0 = pin belongs to its peripheral function |
| 0x1C / 0x20 | GPIO-enable set / clear |
| 0x24 | pull enabled read-back |
| 0x28 / 0x2C | pull enable / disable |
| 0x30 | pull type read-back: 1 = up, 0 = down |
| 0x34 / 0x38 | select pull-up / pull-down |
| 0x3C | keeper read-back |
| 0x40 / 0x44 | keeper enable / disable |
| 0x48 | open-drain read-back |
| 0x4C / 0x50 | open-drain set / clear |
| 0x54 | "IN" register: defined, never used |

Global registers in the same block: 0x2A0 external-interrupt configuration,
2 bits per interrupt n at bit 2n: low bit = polarity (1 = active high /
rising), high bit = mode (1 = edge, 0 = level). 0x2AC edge clear, write
1 << n. (`gpio-dspg.c:39-40`, `:397-445`, `:354`.) "Both edges" is emulated
by flipping the polarity bit after every interrupt.

### 4.3 External interrupt map (live dts, `dspg,extints`)

EXTINT n -> pin -> PLIC id: 0 AGPIO16 (24), 1 AGPIO17 (25), 2 AGPIO18 (26),
3 AGPIO19 (27), 4 BGPIO26 (28), 5 CGPIO28 (29), 6 DGPIO8 (30), 7 DGPIO9 (32),
8 DGPIO26 (33), 9 DGPIO24 (34), 10 DGPIO25 (35), 11 BGPIO31 (36), 12 EGPIO6
(37), 13 DGPIO30 (38), 14 DGPIO31 (39), 15 BGPIO11 (40). EXTINT13 moves to
CGPIO31 when SPCR2 bit 17 is set. The SLIC interrupts are EXTINT0 and EXTINT1.

### 4.4 Pin mux

Two levels:

1. Per pin: the GPIO-enable bit (0x18 read, 0x1C set, 0x20 clear). 1 = GPIO,
   0 = peripheral function.
2. Per pad group: SPCR1 / SPCR2 in the system configuration block (0.5).

There is no pinctrl driver and the HT802 dts selects nothing for SPI or TDM
pins. The settings are made by U-Boot:

| Function | Pins released from GPIO mode | Pad-group bit | Pulls |
|---|---|---|---|
| SPI2 (SLIC bus) | BGPIO13 (DO), BGPIO14 (DI), BGPIO16 (CK) | SPCR1 bit 19 | DO up, DI down, CK up |
| TDM1 | BGPIO0 (bit clock), BGPIO1 (FSYNC), BGPIO2 (data TX), BGPIO3 (data RX) | SPCR1 bit 23 | not changed |

(`U:board/dspg/evb9918/spi.c:35-63`, `tdm.c:33-38`.) U-Boot's CS / reset /
INT pin choices in that file are for an older single-SLIC board (reset on
AGPIO28, INT on AGPIO17) and do not match this unit's dts, except CS on
DGPIO16.

Kernel behaviour worth knowing (`gpio-dspg.c:171-240`):

- Requesting a GPIO whose enable bit is 0 logs a warning and switches the pin
  to GPIO mode.
- Freeing a GPIO clears the enable bit again (a Grandstream addition), which
  hands the pin back to its peripheral function. Unloading a module that owns
  CS or reset GPIOs can therefore change what drives those pins.
- Setting a direction may silently disable the pull resistor (outputs that
  are not open-drain; inputs with keeper on) (`:147-169`).

---

## 5. Addresses, /dev/mem conflicts, locking and ordering traps

`CONFIG_STRICT_DEVMEM` is off in the GPL config (`defconfig_ht80x:1353`), so
/dev/mem can map all of these. `request_mem_region` in the drivers does not
stop a /dev/mem mapping; it only stops a second kernel driver.

| Block | Base, size | Bound kernel driver (GPL config) | /dev/mem conflict |
|---|---|---|---|
| SPI2 (dcx) | 0x05E00000, 0x200 | `spi-dspg-dcx`, built in, IRQ driven | Yes while any kernel SPI client transfers. Safe only when no client is active: keep IER = 0 and poll. A stray end-of-transfer interrupt runs the kernel handler, which writes IER = 0 and may read DAT |
| TDM1 | 0x05F00000, 0x2000 | `dvf-tdm` (`gs-tdm.c`), built in | The ARM driver is passive after probe, but the CSS owns and services the FIFO after a grant. Poking it then corrupts the stream on both sides |
| Watchdog | 0x05700000, 0x100 | none (driver not built) | No driver conflict. Writing the ENABLE key starts an unstoppable 1..1000 s reset timer |
| GPIO | 0x05000000, 0x2B0 | `gpio-dspg`, built in | Set / clear registers are race-free. Do not touch 0x2A0 (kernel does read-modify-write under a spinlock). Pins requested by kernel code can still be driven from /dev/mem; the kernel will not notice |
| CMU | 0x05300000, 4 KiB | clock layer, always | Read-modify-write races with kernel and CSS (0.4) |
| SYSCFG | 0x05200000, 4 KiB | none (GPIO driver touches SPCR2 bit 17 only) | Read-modify-write, low risk |
| DMA | 0x06400000, 0x200 | device registered, no driver | No driver conflict; clock gated, CSS has its own DMA block |

Traps and ordering rules collected:

1. TDM: gate the clock before configuring; reset pulse with clock off;
   ungate; pre-fill / drain; only then enable (2.1, 2.3).
2. TDM watermark interrupts are level-type and fire with the block disabled.
3. TDM IRQ is requested without the shared flag by a built-in driver. A
   second kernel driver cannot take IRQ 60 unless `dvf-tdm` is unbound first
   (its remove function frees the IRQ and the region, `gs-tdm.c:835-846`).
   After a grant the ARM-side IRQ stays disabled for good.
4. Revoke before touching TDM1. If the CSS firmware stops or is reloaded, the
   CSS loader puts TDM1 back into reset and drops the clock references the
   CSS held (`css.c:582-601`), so the block loses its configuration.
5. The grant leaves ENABLE = 0 and INT_EN = 0xF; revoke sets ENABLE = 1 but
   leaves INT_EN = 0xF. A new owner must rewrite both.
6. SPI: FIFO pointer reset and CON rewrite before every burst; bursts are at
   most 32 words; a receive is started by a dummy read of DAT; transfers are
   one direction at a time.
7. SPI chip select in the GPL driver is one fixed GPIO for all devices
   (1.6). Driving DGPIO16 / DGPIO17 by hand through the data set / clear
   registers works regardless of who requested them.
8. Unloading the closed SLIC modules may free GPIOs, which flips those pins
   out of GPIO mode (4.4). Re-assert GPIO-enable before using them.
9. CMU reset / gate registers are shared with the CSS (0.4).
10. Kernel is PREEMPT with `CONFIG_PREEMPT__LL` (`defconfig_ht80x:324-327`);
    the SPI receive path masks interrupts around its start sequence, which
    suggests the start of a single-word receive is timing sensitive.

---

## 6. Open questions (not answered by the source)

1. sysclk and PLL frequencies on this unit, and the TDM1 clock source mux
   (CLKSWCNTRL[20:19]). Needed to know whether the 8 kHz frame rate is exact.
   Read `/proc/clocks` on the unit, or CMU 0x00, 0x04, 0x08, 0x14, 0x1C, 0x2C.
2. Whether the CSS firmware changes the TDM1 clock rate, slot layout or
   CONFIG registers after the grant. A register dump of 0x05F00000..0x3C and
   0x05F01800..0x0C while a call is up would show the real running
   configuration (reads of 0x3C would steal RX samples; do not read it).
3. Which timeslot each SLIC channel uses, and whether the SLIC is set to
   16-bit linear or 8-bit companded inside a 16-bit slot. This is programmed
   by the closed `drv_silabs` / `bsp_ht` modules. The TDM side only says
   4 slots of 16 bits.
4. Alignment of a 16-bit sample inside the 32-bit FIFO word (top 16 bits, or
   elsewhere in the top 20), and the exact effect of CONFIG_2 bit 8.
5. FDA bits [7:6] (always written as 3) and CONFIG_2 bank-size codes other
   than 0 and 1. Whether a 1-slot or 8-slot frame gets a deeper FIFO is
   unknown.
6. FSYNC_DUR encoding: the HT driver writes 0, the ALSA driver writes
   width minus 1. Whether 0 means a one-bit-clock pulse is not stated.
7. CONFIG_1 bit 9 (RX edge) polarity: the two drivers set it oppositely and
   their comments contradict each other ("bit set = falling" in `gs-tdm.c:510`,
   "bit clear = rising" in `dspg-tdm.c:516`).
8. INT_EN versus INT_STATUS naming: enable bits are called watermark / empty /
   full, status bits 1 and 3 are called underrun / overrun, and a comment
   labels empty / full "for normal operation" and the watermarks "to catch
   errors". Which enable bit is the periodic service interrupt is unclear.
9. TDM STATUS (0x30) bit layout, and the registers at 0x0A00 / 0x1200.
10. SPI CON bit 0 (always 1) and bits 6:1; the hardware meaning of the RX / TX
    threshold select bits; what MOSI carries during a receive; why FRM is not
    written for a single-word receive.
11. How the second SLIC's chip select (DGPIO17) is driven on this unit. The
    GPL driver only uses entry 0 of `current_cs_gpio`; the live dts has two
    entries, so the unit's kernel or `bsp_ht` differs from the tree.
12. Whether the unit's kernel includes the watchdog driver and whether any
    stock process opens `/dev/watchdog` (section 3 lists the checks). Whether
    Bootastic or the mask ROM arms the watchdog is unknown; the fact that the
    stock system runs without a kernel watchdog driver in the GPL config
    suggests not.
13. The DMA controller register map was not extracted here
    (`K:drivers/staging/dspg/dmw96dma.c`); needed only if the ARM-side TDM
    path uses DMA instead of the watermark interrupt.
14. Watchdog INTCLR / INTSTAT semantics and what the interrupt-enable bit
    does before the reset fires.
