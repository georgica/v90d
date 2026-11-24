# HARDWARE.md: Grandstream HT802 (this unit)

Facts about the hardware, learned from: the unit's own `/proc` and `/sys` (root
shell), the extracted stock images and boot log, the live board device tree
(pulled from the `factory` flash partition), and the Grandstream HT8xx GPL
source. These are hardware facts (addresses, GPIO numbers, chip IDs); no vendor
driver code is reproduced here.

## Unit identity
- Model **HT802**, hardware **V2.5A**, part number **9610003725A**, MAC c0:74:ad:xx:xx:xx.
- Stock firmware as received: **1.0.65.1** (now 1.0.65.2 on the active `prog`
  slot: our injected recon image; see RECOVERY). IP 192.168.1.172.

## SoC and CPU
- **DSPG DVF9918** (ARM926EJ-S, ARMv5TEJ), 400 MHz. `hw_features/cpu = dvf9918`.
  Confirmed on the unit; this is *not* the DVF9919 variant that newer boards use.
- No FPU, no hardware divide, no NEON (`neon_support=0`). Has ARMv5TE DSP
  multiplies. VIVT caches. One core.
- Second on-chip DSP core "CSS" at 0x08200000 runs the closed voice firmware
  (`css-loader`). Not used by our design.
- **Fixed-point benchmark on this chip** (our `bench.arm`, stripped, pushed and
  run on the box):
  - rfir 25.4, rlms 27.6 MMAC/s (real) · cfir 57.5, clms 47.8 MMAC/s (complex)
  - Modeled receiver budget (EC 2.048M + FE 0.768M + EQ 2×0.819M ≈ 4.45 MMAC/s)
    → **~18 % of one core**. Headroom exists for a V.34/V.90 receiver, though
    the full receiver (Viterbi, timing, carrier, shell demap) adds to this and
    shares the core with whatever telephony code we run.
- **V.90 answering modem on this chip** (`rig/bench_v90`, replay of a 24 s
  recorded call, run from /tmp under the stock firmware): V.8 3.2 %, Phase 2
  8.0 %, Phases 3/4 19.2 %, data mode 32.1 % of the CPU (28.8 % before the
  renegotiation and retrain watches); worst 10 ms block 8.4 to 8.8 ms wall
  clock; output bit-exact with x86.

## Memory
- 32 MiB SDRAM; ~26.5 MiB visible to Linux. System RAM at 0xC8000000–0xC97FFFFF
  and 0xC9D00000–0xC9FFFFFF (`/proc/iomem`). Under stock load only ~2.5 MiB is
  free (gs_ata + CSS consume the rest): relevant only while stock runs; our
  own rootfs will not.

## Flash: EON EN25QX128A, 16 MiB QSPI (JEDEC 1c 71 18)
- Corrected 2026-11-14: an earlier reading had this as a GigaDevice part
  (c84018). Our driver read the ID on the unit: 1c 71 18. Status register
  reads 0x00 (no block protection).
- The stock system leaves the chip in QPI (four lines for everything); U-Boot
  resets it to single-line SPI before it starts a kernel. Commands our driver
  uses: 0x9F ID (single line), 0x38 enter QPI, 0x05 status (bit 0 busy,
  bit 1 write enable latch), 0x06 / 0x04 write enable / disable, 0x20 erase
  4 KiB (0.3 s at most), 0x02 page program (3 ms at most), 0x0B fast read with
  6 dummy clocks.
- Controller values that work (read from the running stock system): CFG
  0x10009, CSTIMING 0x300, RDTRSTP 0x0008362A, RDCMD 0x0B, PPTRSTP
  0x0008048A, PPCMD 0x02, MEMADD 0xFFFF0000. Reads and page programs go
  through the memory window; status, erase and the rest through the
  auxiliary command registers (details in `fw/drv/qspi.c`).
- **Lost page programs (measured).** About one page program in 700 written
  through the window never reaches the chip: it does not go busy, its write
  enable latch stays set, the controller's interrupt status gains bits
  0x830. From then on the controller programs nothing at all, while reads
  and erases still work. Reset 11 of the clock and reset unit (CMU 0x50
  bit 11, set then cleared) followed by writing the values above again
  brings it back; the chip keeps its mode. The driver reads every page back
  and does exactly this before it repeats one. Writing the 2.1 MB image:
  8245 pages, 11 repeated, read back identical (md5). The cause of the first
  lost page is not known.
- Memory-mapped read window at 0x80000000 (`qspic`), controller at 0x05500000.
- Full partition map (`/proc/mtd`, A/B pairs), total exactly 16 MiB:

  | mtd | name | size | mtd | name | size |
  |----|------|------|----|------|------|
  | 0 | bootastic | 16 KiB | 8 | prog2 | 3324 KiB |
  | 1 | factory | 48 KiB | 9 | prog | 3324 KiB |
  | 2 | nvram | 64 KiB | 10 | lang | 128 KiB |
  | 3 | uboot2 | 256 KiB | 11 | lang2 | 128 KiB |
  | 4 | core2 | 1280 KiB | 12 | oem | 128 KiB |
  | 5 | core | 1280 KiB | 13 | uboot_env | 8 KiB |
  | 6 | base2 | 3072 KiB | 14 | uboot | 256 KiB |
  | 7 | base | 3072 KiB | | | |

- `bootastic` (first-stage loader), `uboot` (U-Boot), `factory` (board params +
  **device tree**, DTB at factory offset 0x4000), `core`/`core2` (kernel uImage),
  `base`/`base2` (squashfs rootfs), `prog`/`prog2` (squashfs mounted at /app),
  `lang`/`oem` (localization/OEM), `nvram`/`uboot_env` (settings).
- Flash is AES-container + 32-bit checksum only; **not public-key signed**
  (see the firmware note below). `hw_features/default_fwkey = nokey` → the stock
  AES key (37d6ae8b…) is in effect.

## Peripheral map (`/proc/iomem`, device tree, live `/proc/interrupts`)
| Block | Base addr | Linux IRQ | Compatible |
|---|---|---|---|
| GPIO controller | 0x05000000 | 18–33 | dspg,gpio (5 banks) |
| interrupt controller (PLIC) | 0x05100000 | - | dspg,plicu |
| QSPI flash controller | 0x05500000 | 42 | dspg,qspic |
| UART (console) | 0x05b00000 | - | DW_UART / dspg-uart, 115200 8N1 |
| SPI (SLIC bus) | 0x05e00000 | 41 (edge) | dspg,spi-dcx, 1 MHz max |
| TDM0 (PCM, active) | 0x05f00000 | 43 | dspg,tdm (reset line 9) |
| TDM1 | 0x06000000 | - | dspg,tdm (disabled) |
| dvf9a (analog/DECT) | 0x06300000 | 34 | dspg,dvf9a (disabled) |
| Ethernet MAC | 0x06e00000 | 40 | st,spear600-gmac (stmmac) |
| crypto | 0x06c00000 | 35 | dspg-crypto |
| CSS DSP core | 0x08200000 | - | closed; not used |

## GPIO assignments (from the board DTB)
- **LEDs** (bank/pin numbers): LED_WAN=24, LED_RJ45=25, LED_PHONE1=26, LED_PHONE2=27.
  All four are **active low** (seen on the unit 2026-11-15: driving the lines to 1 makes them dark).
  The SLIC's battery reads 58.6 V on hook and 21.6 V off hook with the Conexant modem on port 1;
  the echo of our own signal comes back about 9 dB down; the line driver's round trip is 181 samples.
- **Reset button**: GPIO 22.
- **SLIC reset**: SLIC0 = GPIO 21, SLIC1 = GPIO 19.
- **SPI chip-selects** (GPIO-driven): GPIO 16 and GPIO 17.
- **SLIC interrupt**: GPIO lines → Linux IRQ 46 and 47 (`Silabs_int`, edge).
  (The V1.x boot log showed different pins; this V2.5A board's values above are
  from its own device tree and `/proc/interrupts`.)

## Register-level facts for our own drivers (2026-11-14)
Sources: reading the vendor GPL kernel and U-Boot (facts only, full notes in
`refs/dvf99-io-facts.md`) and live reads on the unit with `fw/tools/peek`.

- **GPIO numbering correction.** The device tree cells are (bank, pin), bank
  A=0 .. E=4 (later banks follow), Linux number = bank*32 + pin. So: SPI chip
  selects DGPIO16 and DGPIO17 (Linux 112, 113), SLIC resets EGPIO21 (SLIC0)
  and EGPIO19 (SLIC1) (Linux 149, 147), SLIC interrupts AGPIO16 and AGPIO17.
  The bare numbers in the section above are pins inside those banks.
- **GPIO block** 0x05000000 + bank*0x60: data 0x00, set 0x04, clear 0x08,
  direction read 0x0C (1 = input), make-output 0x10, make-input 0x14, GPIO
  enable read/set/clear 0x18/0x1C/0x20 (0 = pin belongs to a peripheral).
  Live: bank B enable 0x03FE1F00 (BGPIO0..3 = TDM1 pins and BGPIO13/14/16 =
  SPI2 pins are peripherals), bank D data 0x04EF4241 with pins 16, 17 outputs.
- **Pin mux** is set only by U-Boot: SPI2 on BGPIO13 (DO), 14 (DI), 16 (CK)
  with SYSCFG 0x05200054 bit 19; TDM1 on BGPIO0..3 with bit 23. Live value
  0x24884000. Our kernel must not depend on it staying so; it sets it itself.
- **SPI2** 0x05E00000, 16-bit registers: GLOBAL 0x00, CON 0x04, FRM 0x08, IER
  0x0C, STAT 0x10, DAT 0x14; FIFO 32 words; mode 3, 8-bit: CON = 0x0F81 (live
  value the same). Clock from sysclk through CMU 0x05300028 bits 31:24 (live
  0xC7), gate 0x0530005C bit 7, reset 0x05300054 bit 7. Chip selects are
  active-low GPIOs driven by software.
- **TDM1** 0x05F00000 (the device tree calls it TDM0), 32-bit registers:
  ENABLE 0x00, CONFIG_1 0x04, CONFIG_2 0x08, FSYNC 0x10, FSYNC_DUR 0x14,
  FIFO_WM 0x18, INT_EN 0x20, INT_CLR 0x28, INT_STATUS 0x2C, TX_FIFO 0x38,
  RX_FIFO 0x3C, slot descriptors from 0x1800. Live values in a running stock
  system: ENABLE 1, CONFIG_1 0x1A03, CONFIG_2 0x0301, FSYNC 0, FSYNC_DUR 0,
  FIFO_WM 0x3030, INT_EN 0x0F; the slot descriptors read back as 0.
  The SoC is bit clock and frame sync master: 4 slots of 16 bits, 64 bits per
  frame. The FIFO is 128 words each way: 4 ms. In the stock system the closed
  DSP core services it (after `/proc/gs/css_own_tdm` is written 1); the ARM
  never moves a sample. Our driver has to do it in a kernel interrupt handler
  (hardware IRQ 60) or by DMA; a user-space poll cannot keep up.
- **The 8 kHz clock.** Measured from the registers: mainclk 25 MHz -> PLL3
  (0x05300008 = 0x00045901: /2, x73, /2) = 456.25 MHz -> audclk -> TDM1
  divider 891 (0x0530002C bits 11:0 = 0x37A; source select 0x0530001C bits
  20:19 = 1) = 512 065.1 Hz bit clock = **8001.02 Hz** frame rate, +127 ppm
  from nominal. This is what every stock HT802 runs at, so a modem on the port
  already sees it today. An exact 8000 Hz needs PLL3 at a multiple of
  512 kHz (25 MHz * 64 / 3125): for example /25, x128, /2 and divider 125;
  whether the VCO locks at 128 MHz is unknown, so the first build keeps the
  stock values. No DECT crystal is assumed (the stock clock tree makes its
  13.825 MHz from PLL3 too).
- **Resets and gates.** Block reset 0x05300050 bit 9 and clock gate
  0x05300058 bit 9 for TDM1 (live 0x00040020 and 0xCED00B2E). Watchdog at
  0x05700000 starts only when `/dev/watchdog` is opened.
- Interrupts seen live: 41 `spi-dspg-dcx` (edge, 24 k counts), 42
  `dspg-qspic`, 43 `dvf_tdm_irq` (1 count).

## SLIC / codec: the main feasibility risk
- **Correction, measured on this unit 2026-11-14: the SLICs are two
  single-channel Si3217x ProSLICs, not one dual Si32260.** The kernel log of
  this V2.5A unit says "Si3217x: Channel 0 : Type = 26", "Rev = 2" (rev C),
  "loading patch: 06182014", "VBAT Up = 58.967 v (Vbath = 59.985 v)", port 1 on
  PCM slot 0, port 2 on slot 1. Register 0 reads 0xB2 on both chip selects.
  The "Chip Type 28" lines below came from somebody else's V1.x boot log.
- Stock configuration read back from the working part with our own SPI code
  (`fw/tools/slicpoke`, snapshot in `recon/out/slic0_stock_state.txt`, and the
  reset-default state in `recon/out/slic0_reset_state.txt`): PCMMODE 0x13
  (16-bit linear, enabled), PCM start counts 0 (port 1) and 16 (port 2),
  LINEFEED 0x11 (forward active), IRQEN2 0x13, IRQEN3 0x03, AUTO 0x3E, ENHANCE
  0, PDN 0x80, patch jump table enabled (JMPEN 1, six entries), PCLK_MULT 4.
  RAM: MADC_VBAT 58.6 V, VBATH_EXPECT 60 V, PD_DCDC 0x00400000, converter
  drive polarity word 0, patch ID 0x06182014.
- The stock driver touches the SPI bus only on telephone events (interrupt
  count constant over 5 s idle), so the snapshot was taken with the stock
  system running. The stock driver also prints a register dump to the kernel
  log when `/proc/driver/si32260-cpe/rd0` is read.
- Our polled SPI code works: every octet is its own chip-select frame; send =
  FIFO pointer reset, CON 0x0F81, write DAT, wait for busy to clear; receive =
  one dummy read of DAT, then CON with bit 13, wait for busy to clear, read
  DAT. From user space about one access in fifteen is spoiled when an
  interrupt lands between the dummy read and the CON write; the kernel driver
  does those two with interrupts off.
- **Silicon Labs Si3226x dual ProSLIC** (`slic_vendor=0`, `si3217x_dev=1`,
  `num_slic_dev=2`, `num_ch_per_slic=1`, `slic_dcdc_type=2`, `slic_hw_prot=1`).
  Boot log: "Chip Type 28" (→ Si32260), "Chip Rev 3" (rev C), DC-DC "LCQCUK".
  FXS port 1 (our target) is channel 0.
- On SPI @ 0x05e00000 with raw 8 kHz PCM on the TDM bus (TDM0). Devices present:
  `/dev/fxs00-03`, `/dev/voice0-9`, `/dev/dvf9a`.
- How the stock system moves PCM (seen on the running unit, 2026-11-14): no
  ALSA (`/proc/asound` and `/dev/snd` do not exist). Character devices
  `ifx_tapi (fxs)` 121, `slic_bsp` 122, `voice` 253, `coma-sharedmem` 254;
  loaded modules `drv_silabs`, `drv_tapi`, `bsp_ht` (closed). The TDM block is
  claimed by the kernel as `dvf-tdm` (0x05f00000, 8 KiB) with IRQ 43
  `dvf_tdm_irq`, which had fired once in hours of uptime: samples do not go
  through per-interrupt handling on the ARM. A DMA controller (`dmw96dma`,
  0x06400000) and the CSS shared memory are present. So user space on the
  stock kernel has no plain PCM device; our SLIC/TDM driver has to be our own
  (kernel, or /dev/mem during bring-up).
- What the stock kernel allows from a root shell (2026-11-14): modules load
  (`sys_init_module` present, three vendor modules loaded, no `/lib/modules`
  tree), `/dev/mem` exists, the SPI core and `spidev` are built in, there is
  no kexec and no `/proc/config.gz`. So our SLIC and TDM code can be brought
  up on the running stock kernel from RAM (a module built against the 3.4.20
  tree, or a /dev/mem tool) after stopping `gs_ata` and unloading
  `drv_silabs` / `drv_tapi`; a reboot puts everything back. No flash and no
  UART needed for that. A kernel of our own cannot be started from here.
- **Blocker for a fully-original driver:** the stock init loads a ~900-byte
  RAM/PRAM "patch" (a DSP firmware blob) into the SLIC on every start, and the
  Si3226x register/RAM map is NDA-only (no public datasheet beyond the pin/
  electrical sheet). Whether a rev-C part does battery feed / hook / ring / PCM
  *without* the patch is undocumented and unverified. This is the one part of the
  original-firmware goal that is not clearly achievable from public information.

## Ethernet
- Synopsys GMAC (stmmac, "st,spear600-gmac") at 0x06e00000, IRQ 40/23.
- PHY: **Motorcomm YT8512H**, `eth_interface=RGMII`, `eth_clock_source=Internal`
  (from `hw_features`). The generic DTB lists `phy-mode="rmii"` with a fixed-link
  100/full; the live hw_features value (RGMII/YT8512H) is the board truth and a
  V2.x change from the V1.x KSZ8081/RMII units.

## Our own kernel on the unit (2026-11-14, night): measured facts
Everything here was seen on the unit with our Linux 6.6.63 (`fw/`), started
from RAM; nothing was written to the flash.

- **The SLICs are two single-channel Si3217x rev C** (register 0 = 0xB2), one
  per port, each with its own chip select and reset line, not one Si32260.
  The sections above that say Si32260 describe the stock driver's name only.
- **Port 2 comes up the same way (2026-11-15, our kernel from flash).** Second
  Si3217x on chip select DGPIO17, reset EGPIO19, PCM start count 16 (slot
  1): id 0xB2, all calibrations finish, battery 58.7 V, on hook with nothing
  plugged in, at the same moment as port 1 (both parts started in parallel,
  30 ms apart in the log). The codeword of port 2 is bits 31:24 of the second
  FIFO word of a frame. With both ports running the highway counters stand
  still (17 underruns and 16 overruns counted once, during start-up, none in
  the following minutes). Not shown: off hook, ringing or audio on port 2.
- **No vendor patch needed.** `fw/drv/slic.c` brings port 1 up from reset
  without the RAM patch the stock driver loads: all calibrations finish,
  battery 58.6 to 58.8 V, line feed forward active, hook reads on hook.
- **TDM FIFO word layout.** 4 words per frame, one per slot; port 1 is slot 0
  (PCM start count 0), port 2 slot 1 (count 16). A 16-bit linear sample sits
  in bits 31:16. In companded mode (PCMMODE 0x11 mu-law, 0x10 A-law) the
  codeword is bits 31:24 in both directions; bits 23:16 read 0xFF. Unused
  slots read 0xFFFF0000. The receive watermark interrupt is level type.
- **The two TDM FIFOs must stay paired, and do not by themselves.** After
  any time with interrupts off long enough for the transmit FIFO to empty or
  the receive FIFO to overflow (a kernel log line on the serial console:
  about 14 ms), part of every 8-frame chunk written afterwards is lost on
  its way to the line, permanently, while the receive direction stays clean
  and no counter moves any more. Only a fresh start of the block (gate,
  reset, prefill) heals it; the driver does that on every underrun or
  overrun it sees. Both SLICs ride through such a restart without losing
  their settings. Measured 2026-11-15; PROGRESS.md has the numbers.
- **The 8 kHz clock, retuned.** PLL3 at /2, x29 (register low bits 0x0381,
  362.5 MHz, inside the VCO range the vendor code allows) and TDM divider 708
  give 511 994.35 Hz: 8 kHz - 11 ppm instead of the stock + 127 ppm. It locks
  (bit 18) and the SLIC runs on it. An exact 8 kHz would need a phase
  detector frequency of 5 MHz, below the 10 MHz the vendor code allows, so it
  is not used. Measured: 8000 frames per second of kernel time (same crystal,
  so this checks the divider arithmetic, not the crystal).
- **Polled SPI to the SLIC.** Each octet is its own chip-select frame and
  must not be interrupted between its register writes: the kernel driver
  does an octet with interrupts off (about 40 us). 0 errors in use.
- **Watchdog (0x05700000).** Period register 2 = 100 s, enable = 0xABCD
  twice, refresh = 0xA5A5 then 0x5A5A, as in refs. Found on the unit:
  **writes that follow each other at CPU speed are not taken properly**: a
  driver writing the refresh pair back to back had the unit reset 3 to 6 s
  after its probe; with 2 ms after every write it runs indefinitely. The
  enable register reads 0 whether it runs or not. Re-arming a running
  watchdog and rewriting the same period are harmless.
  Started by code of ours that runs before our kernel (the loader, entered
  from RAM out of the stock system, 2026-11-15) it reset the box 2 to 3 s
  after every refresh, with the same values and 2 ms pauses, also with 20 ms
  and 1 s pauses, with the instruction cache off, and whether or not the
  stock side had started it before. Started from the stock shell before
  the jump and then left alone until our kernel's driver takes it over, it
  does not do this. Cause not established (the clock it
  counts on is the suspect). Nothing of ours touches it before the kernel
  driver now.
- **RAM does not survive a reset**: the 5 MiB hole at 0xC9800000 comes back
  as zeros or a 0xDEAD0100 test pattern, so a RAM console (ramoops) cannot
  carry a crash log across a reset. The bring-up build uses the kernel's
  network console instead (`make DEBUG=1 KARGS='netconsole=...'`).
- **UART1 (0x05A00000) is the console**, 115200; its clock gate (bit 6) is
  off under the stock kernel and must be set before a RAM boot.
- **RAM boot without a serial cable** (`fw/tools/ramboot.sh`): from the stock
  firmware's root shell, stop the telephony processes, hold the DSP core in
  reset, stream the image into the unused RAM hole with `peek -l`, arm the
  watchdog, and load `fw/tools/kjump` (a module laid out by hand for the
  stock kernel 3.4.20-rt31 of firmware 1.0.65, which calls that kernel's own
  restart-into-address routine). An image that hangs ends in a watchdog reset
  and the stock firmware is back. `reboot` in our system also returns to
  the stock firmware.
- **U-Boot keeps a saved environment** in the 8 KiB `uboot_env` partition at
  flash offset 0xFBE000 (hush parser on): CRC32 (little endian) and 8188
  octets of `name=value` strings, one copy. On a stock unit it is erased and
  the built-in default (`bootcmd=gsboot`) is used. Shown on the unit: U-Boot
  loads a saved one, runs `if test` / `setenv` / `saveenv` / `bootm` from
  `bootcmd`, and `saveenv` writes it back. This is how our image starts from
  flash (DESIGN.md).
- **Flash layout as used now** (physical offsets): our settings 0x060000
  (64 KiB, start of stock `core` copy A), our image 0x2E0000 (3 MiB, stock
  `base` copy A), environment 0xFBE000. Both boot loaders, `factory` and
  stock copy B (core 0x1A0000, base 0x5E0000, prog 0xC1F000) are identical
  to the backup (md5 per partition, taken after all writes). The stock
  system itself rewrites `nvram` and `lang2` at every boot.

- **Flash layout of the release scheme** (designed 2026-11-15, not yet on a
  unit; X is the start of the `core` of the set our file was installed
  into, 0x060000 for set A or 0x1A0000 for set B):
  - X + 0x000: stock header (signature at X), X + 0x200: our loader as a
    U-Boot image (about 6 KiB).
  - X + 0x10000: the loader's count of unconfirmed starts (one word, one bit
    cleared per start; the sector is erased to confirm).
  - X + 0x20000, 64 KiB: our settings.
  - `base` of the same set (0x2E0000 or 0x5E0000): stock header, at + 0x200
    the system image.
  - `prog` of the same set: stock header and a few octets of text.
  The stock boot loader reads only the length each header names, so the
  count and the settings inside `core` do not enter its checksum.
- **What the stock upgrade erases**: the whole target partition of every
  file it installs (`core` 1280 KiB, `base` 3 MiB, `prog` 3324 KiB of the set
  that is not running), nothing else. So anything of ours inside our set is
  gone after any stock install into it.
- **Partitions outside the two sets, as found in the backup**: `oem` starts
  with a gzip stream (1f 8b 08, 247 octets in use), `lang2` likewise (75
  octets); `lang` and `uboot_env` are erased; `uboot2` is a second boot
  loader image with a stock header (install counter one lower than
  `uboot`). None of them is free for our use.
- **Writable ranges of our kernel's flash driver** (device tree
  `v90d,writable`): 0x060000..0x071000, 0x080000..0x090000,
  0x1A0000..0x1A1000, 0x1B0000..0x1B1000, 0x1C0000..0x1D0000,
  0x2E0000..0x8E0000, 0xFBE000..0xFC0000. A request must lie inside one
  range. MTD devices: `all` (the whole chip, the one `v90nv` uses) and the
  three bench partitions `v90d-conf`, `v90d-image`, `uboot-env`.
- **Stock `core` is nearly full** (0x135E00 of 0x140000 octets used in
  1.0.65), so the offsets above are stock kernel data in a stock set: they
  mean something only in a set whose `core` holds our loader.

## Linear PCM and the gain knobs (2026-11-15; offline work, nothing of it measured yet)

Why: the codec ran in G.711 in both directions, so what we receive was
companded to 8 bits together with the echo of our own transmitter (9 dB
down, about -21 dBm0, as loud at the codec as the caller). The quantiser's
steps at the echo's peaks are the noise floor of the upstream. The part has a
16-bit linear PCM format, and the stock firmware ran it that way.

### What the map and the unit say

- Register 11 PCMMODE: bits 1:0 the format (0 A-law, 1 mu-law, 3 16-bit
  linear), bit 4 PCM enable, bit 2 A-law inversion, bit 5 tri-state
  (`refs/si3217x-map.md` 4.5). **One format for both directions**: there
  is no "linear receive, companded transmit". So in linear mode the driver
  sends, for every codeword the daemon hands it, the exact G.711 value of
  that codeword as a 16-bit word (`pump_lin16` in `fw/drv/pump.c`, the same
  numbers as `v90/v90_pcm.c`, tested for all 512).
- Slots: the start counts are in bit clocks from frame sync and do not
  change with the format: port 1 at 0, port 2 at 16, a sample 16 bits long
  in a 16-bit slot of the 4 x 16 frame. This is exactly what the stock
  firmware had (PCMMODE 0x13, counts 0 and 16; section above). The second
  octet of a slot, which carries ones in companded mode, carries the low
  octet of the sample.
- Bit order: most significant bit first; a sample is bits 31:16 of the SoC's
  FIFO word ("TDM FIFO word layout" above).
- The band filters, the high-pass switch (DIGCON bits 2 and 3, which we set
  as the stock firmware does) and the AC gains are not part of the PCM
  format in the map: the setup routine writes them the same for every
  narrowband format. Nothing there says the analogue level of a digital
  value differs between the formats.
- RAM 1002 LINEAR_OS (rev C only) and RAM 883 ALAW_BIAS: the map has their
  names and nothing else. Both read the same after reset and in the running
  stock system (which was in linear format): 1002 = 0x00320000, 883 =
  0x00010000. We leave them as the part resets them.

### What is assumed (the first live run has to confirm it)

A1. A 16-bit linear sample is two's complement on the 16-bit scale of the
    G.711 tables: it equals the decoded value of the codeword the part
    would have used in companded mode, mu-law 14-bit value x 4 (+/-32124
    at the top codeword), A-law 13-bit value x 8 (+/-32256). So 0 dBm0 is
    15770 RMS in both formats and both directions, and the modem, which
    works on this scale already (`v90_octet_linear`), takes the sample as it
    is: no shift, no bits dropped. (The two laws differ by 0.03 dB at full
    scale; in linear mode the law only selects the table for transmit.)
A2. LINEAR_OS at its reset value gives no DC offset worth mentioning.
A3. The formats share the analogue gains (TXACGAIN, RXACGAIN).
A4. V.90 does not depend on A1 being exact: the caller's modem learns the
    levels from the DIL. It needs the 255 levels to stay distinct and
    monotonic, which any uniform scale gives.

### What the unit says (measured 2026-11-15, port 2, Conexant modem on the line)

- The switch works: with the daemon asking for linear, register 11 reads
  0x13, the statistics say `rx_format linear`, the kernel log "port 2:
  mu-law, linear receive"; calls connect in that format.
- **A1 is wrong for what we receive, right for what we send.** The same
  signals (the modem's DTMF, its V.8 tones, the echo of our ringback and
  answer tone) come out of the codec 10.9 dB lower in linear format than its
  compander encodes them: four stretches of two recordings, 22.9 dB apart on
  a scale that had the companded one 12.04 dB high, every time; the driver's
  meter says the same (dial tone echo: mean square 3486182 companded, 285564
  linear). That is a factor of 2/7 in amplitude (10.88 dB). The echo and
  the far end's signals differ by the same amount, so the level we put on
  the line is the same in both formats. The daemon multiplies the linear
  samples by 7/2 (`line_lin_sample`, srv/line.c), which puts them on the
  scale the modem works on and keeps the finer steps: at the echo's peaks
  the compander's step is 128 to 256 units, the corrected linear one 3.5.
- A2: on hook both formats read 0 (peak 0: the part mutes). Off hook with
  dial tone playing, `rx_dc` -6 linear, -11 companded: nothing to correct.
- A3: not separated from A1; the 10.9 dB could be a gain the linear format
  leaves out. Left as found.
- A4: V.90 and V.92 connect in linear format.

Rates, one call each, the same hour (downstream/upstream):

| call  | companded     | linear        |
|-------|---------------|---------------|
| V.90  | 50666 / 26400 | 52000 / 28800 |
| V.92  | 50666 / 25333 | 50666 / 30666 |
| V.34  | 31200 / 28800 | 31200 / 31200 |

The daemon now asks a dev: line for linear by itself (`-R` overrides).

### How to check on the box

The driver meters what it receives, as linear values in either format
(a companded codeword is decoded first): `rx_dc`, `rx_peak`,
`rx_mean_square` over the last 1.024 s in
`/sys/bus/platform/drivers/v90line/*/stats` (`stats2` for port 2), next to
`rx_format`. 10 log10(rx_mean_square / 15770^2) is the level in dBm0.

1. Format switch works: `v90d ... -R l` (or `echo -n l > /dev/v90line`
   while no daemon has it open: the device is single-open, so stop the
   daemon first) then `slicpoke kreg 11` must read 13, `stats` must say
   `rx_format linear`, and the kernel log "port 1: mu-law, linear receive".
   Back with `-R c`: 11.
2. A2 and the bit order, on hook, nothing plugged in or the modem on hook:
   in both formats `rx_dc` within a few units of 0 and `rx_peak` small
   (idle noise; companded cannot show less than its smallest step, 8). A
   wrong octet order shows as a peak in the thousands.
3. A1 and A3, transmit and receive scale together: take the port off hook
   (ATH1 on the modem) without dialling: the daemon plays dial tone, and its
   echo comes back. Read `rx_mean_square` in format c and in format l: they
   must agree within 0.2 dB (5 %). A factor of 4 (12 dB) or 2 would mean a
   different scale in one direction.
4. Receive scale alone: a call in each format; during V.8 / Phase 2 the
   caller's level is the same signal both times. `live/replay` of the two
   recordings (`-w`), or the status page's receive level in a V.21 call
   (our transmitter is in the other band and equally loud both times).
5. Transmit scale alone: the caller's modem reports its receive level
   (Conexant: `AT#UD` / `ATI6` after the call); it must be the same in both
   formats. And V.90 downstream must reach the rates it reaches companded
   (49333 to 52000 on this line): a wrong transmit table would cost rate.
6. `pump_underruns`, `tdm_underruns`, `stale_writes` must stand still in a
   linear call as in a companded one (the interrupt does a table look-up
   more per sample).

### Device format

`/dev/v90line`, `/dev/v90line2`: a read gives 81 octets (hook, 80 codewords)
in companded format and 161 octets (hook, 80 samples, 16 bits, little
endian) in linear format; the reader offers 161 and tells by the length. A
write of 80 octets is 80 codewords in both. One-octet writes: `u` / `a` the
law, `l` / `c` linear or companded. A change restarts the port's pump: do it
on hook. The driver starts companded, so a daemon that knows nothing of
this works as before.

### The AC path in the part's RAM

The part's names: **TX is line to PCM (its A/D: what we receive), RX is PCM
to line (its D/A: what we send).** (The usual ProSLIC sense; the map does not
spell it out. Check: `slicpoke dac -6` must lower the echo in
`rx_mean_square` and what the caller's modem receives by 6 dB;
`slicpoke adc 3` must raise `rx_mean_square` by 3 dB and leave the caller's
modem untouched.) All words are 29-bit two's complement; read as Q27
(0x08000000 = 1.0) the equaliser's first coefficient is 0.99, which is why
that reading is used here. The values are what `fw/drv/slic.c` loads
(`impedance[]`, `impedance2[]`: the stock firmware's set for this port, read
back from the working part).

| RAM | Name | Value | as Q27 | What |
|---|---|---|---|---|
| 540..543 | TXACEQ_C0..C3 | 0x07F53B40, 0x000DD6F8, 0x000105B8, 0x1FFDB9F8 | 0.99474, 0.00676, 0.00050, -0.00111 | A/D path equaliser |
| 544 | TXACGAIN | 0x0A5DB3A8 | 1.29575 | A/D path gain |
| 545 | RXACGAIN | 0x018AC298 | 0.19275 | D/A path gain |
| 906 | RXACGAIN_SAVE | 0x018AC298 | 0.19275 | copy of 545, written with it |
| 546..549 | RXACEQ_C0..C3 | 0x07EE0B68, 0x0012CC80, 0x1FFE61F0, 0x1FFCBFF8 | 0.99123, 0.00918, -0.00079, -0.00159 | D/A path equaliser |
| 563..570 | ECFIR_C2..C9 | 0x001E4E80, 0x1FB8F500, 0x01F8E080, 0x0024BB80, 0x021DEF00, 0x1F249180, 0x006C7580, 0x1FD9A500 | 0.01480, -0.03469, 0.24652, 0.01794, 0.26462, -0.10714, 0.05296, -0.01873 | hybrid balance, FIR |
| 571..574 | ECIIR_B0, B1, A1, A2 | 0x0008B200, 0x1FF2A680, 0x02577380, 0x0558D480 | 0.00425, -0.00652, 0.29270, 0.66837 | hybrid balance, IIR |
| 653..657 | ZSYNTH_B0, B1, B2, A1, A2 | 0x007F8C00, 0x1F01CE80, 0x007EA600, 0x0FF66D00, 0x18099280 | | impedance synthesis (not a gain knob; with reg 45 RA = 0x75) |
| 1002 | LINEAR_OS | 0x00320000 (reset = stock) | | see above |

Gain in dB to a value (map 4.4: how the part's own gain routine places a
change; that routine works in whole dB, a 0.1 dB variant exists):

    new = round(table value x 10^(dB / 20)), as 29-bit two's complement

- D/A path (what we send): attenuation goes into RXACGAIN and its copy
  (545 and 906), with RXACEQ at the table values; gain goes into the four
  RXACEQ coefficients, with RXACGAIN at the table value.
  -3 dB: 0x01177801; -6 dB: 0x00C5D947; -9 dB: 0x008C10F1; -12 dB: 0x006328C5.
- A/D path (what we receive): gain goes into TXACGAIN (544), with TXACEQ at
  the table values; attenuation goes into the four TXACEQ coefficients.
  +1 dB: 0x0BA1809B; +2 dB: 0x0D0CD007; +3 dB: 0x0EA47412. The word ends at
  0x0FFFFFFF, 3.77 dB above the table value; the map speaks of a "normal
  maximum" for this gain without giving it. What the A/D does with a signal
  above its range is not known either: watch `rx_peak` (32767 = clipped at
  the PCM word).
- `slicpoke dac DB` and `slicpoke adc DB` do exactly this (tenths of a dB
  allowed); `slicpoke gains` shows the whole set next to the table values.

`fw/tools/slicpoke` with our driver running: the old commands (`reg`, `ram`,
`wram`) drive the SPI block from user space and would cut into the driver's
own accesses. `kreg`, `kram`, `kwram`, `gains`, `dac`, `adc` go through the
driver's `slic` / `slic2` attribute instead (the port's thread does the
access between two looks at the hook, within 10 ms), at any time, a call
included. Whether the part takes a change of these words cleanly in the
middle of a signal is not known: change them between calls for a
measurement. A restart of the port by the driver (after a fault of the
part) loads the table values again.

### Bench experiments

(a) Lower D/A gain, higher announced power. The caller's modem chooses the
    downstream constellation within the power INFO0d allows (`v90d -P CODE`:
    -0.5 dBm0 less 0.5 dB per step; default 23 = -12 dBm0). With
    `slicpoke dac -6` and `-P 11` (-6 dBm0) the line can see the same power
    as before from codewords 6 dB larger: more of the G.711 levels lie below
    the largest point, so the caller's modem has more points to choose from.
    Whether it uses the allowance is its decision. `slicpoke dac -6` alone
    lowers our echo at the codec by 6 dB, and the caller's receive level
    with it.
(b) Higher A/D gain: `slicpoke adc 3`. Raises caller and echo alike at the
    codec; it helps against noise behind the A/D (in companded format:
    nothing, the quantiser's steps scale along; in linear format: the 16-bit
    step and the part's own noise).
(c) Hybrid balance, below.

### Hybrid balance from a measurement (procedure; no code yet)

The map documents the ECFIR / ECIIR words by name and load order only: not
the tap spacing, not the IIR's form, not where in the path the estimate is
subtracted. So nothing can be computed from an impulse response directly.
What can be done is to measure what each word does:

1. The daemon already has the echo path as an impulse response at 8 kHz:
   the canceller's taps after training (`ec.h` of `p34d` / `v92d`, 96 taps
   after the bulk delay). A quiet, repeatable way to get it without a
   caller: port off hook into the modem (ATH1, no dialling), the daemon
   sends a few seconds of white PCM noise at -15 dBm0 and adapts the
   canceller; in linear format its estimate is not limited by the
   quantiser. Call the result e0 (a vector of 96).
2. For each of the eight FIR words k: add a small step d (say 0.02 in Q27,
   0x0028F5C3) to ECFIR_Ck with `slicpoke kwram`, measure again: ek. The
   column sk = (ek - e0) / d is the echo response to that word. If the
   part's balance filter is a plain FIR in the path, each sk is the same
   short pulse (the codec filters' response) shifted by one tap spacing,
   and the first measurement shows whether that spacing is 125 us or
   62.5 us. Superposition can be checked with two words stepped at once.
3. Solve the 96 x 8 least squares problem min |e0 + S c|^2 for the
   correction c (normal equations, 8 x 8), write table + c, measure, and
   repeat once or twice (the IIR words held fixed).
4. The IIR words (the slow tail) are not linear in the response: treat them
   the same way with numerical derivatives (Gauss-Newton on all 12), with a
   step limit, and only if the FIR alone leaves a tail. Keep |A1|, |A2|
   changes small: a pole outside the unit circle is an oscillator on the
   line.
5. Figure of merit: the echo's mean square with the noise source running
   (`rx_mean_square`), before and after; today it is 9 dB below the
   transmitter. Every dB gained here is a dB of receive range in companded
   format and less work for the canceller in both.
The ZSYNTH words set the impedance the line sees, not the balance; they are
left alone.

## Bootloader
- bootrom v2.0 → **Bootastic** v1.2.6.1 → **U-Boot "dvf99 v1.2.6.1-rc2"**
  (evb9918-ht). Interruptible: "Hit Esc key to stop autoboot" (bootdelay 1).
- `gsboot` validates only the Grandstream magic 0x23C97AF9, a 16-bit checksum,
  and version/part-generation compatibility: **no RSA/secure-boot** in the GPL
  source (`CONFIG_DVF99_SECURE_BOOT` commented out); OTP secure-boot exists on
  the SoC but the config suggests it is not burned. Commands include `tftpboot`,
  `bootm`, `loady`, `qspi read/write.partial`. Default net env ipaddr
  192.168.0.240 / serverip 192.168.0.1, loadaddr 0xC8800000, devtree 0xC8C00000.
- **Not yet confirmed on this unit's live UART** (no serial adapter attached);
  the above is from the extracted bootloader image + GPL source.
- **gsboot, read in full (2026-11-15)**: from the GPL `cmd_gsboot.c` and, to
  be sure the unit runs the same logic, from a disassembly of this unit's own
  U-Boot partition (same structure function by function). Offline work, not
  yet watched on a UART.
  - It checks six partitions: core, base and prog of set A and of set B
    (not `uboot`). A
    partition is good when its first word is the magic and the 16-bit sum of
    `SizeUsed` octets after the 512-octet header plus the header's checksum
    field is 0. Nothing else is looked at: no cpu text, no model key, no
    hardware bits, no image id. `SizeUsed` is not bounded against the
    partition, so a header with the magic and a wild length makes the sum
    run over whatever follows (see the upgrade notes below).
  - All six good: the set whose `prog` header names its own core and base
    versions (fields 0x28 and 0x2C) is "compatible". Exactly one compatible
    set: that one. Both or neither: the set with the higher provision
    counter (0x30, highest of its three), then the higher version (prog,
    base, core in that order), then A.
  - Not all good: A if A's three are good, else B if B's three are good,
    else further cases for partly good sets that we did not need and did
    not model.
  - Then it sets bootargs (`root=/dev/mtdblock6` or `7`, `bootpartition=0`
    or `1`, `partgeneration=1`, the mtdparts list) and runs
    `qspi read.partial core|core2 ${loadaddr} 0x200`, reads the device tree
    from `factory` to `${devtree_addr}`, `bootm ${loadaddr} - ${devtree_addr}`.
    The whole core partition is read to 0xC8800000 from offset 0x200.
  - bootm wants a legacy U-Boot image (magic 0x27051956, header and data
    CRC good) of type kernel, not compressed. It copies the data to the
    image's load address unless they are the same place, and enters at the
    entry address with r0 = 0, r1 = machine number, r2 = device tree address.
    The device tree has the bootargs written into `/chosen` by then.
  - State at the jump: U-Boot turns the data cache on early and this build
    does not turn it off again before the jump, so the code is entered with
    MMU and both caches on over a flat 1:1 map. The flash controller is put
    back to plain single-line SPI mode before the jump. No watchdog is
    started by U-Boot.
  - After the copy (or with no copy) bootm calls its cache flush over
    `load` to `load + 4 * length` (the length is multiplied by four in this
    build). The routine (ARM926 clean-and-invalidate per 32-octet line, then
    drain the write buffer) returns without doing anything when either end
    of the range is not a multiple of 32. So an image whose length is not a
    multiple of 8 is entered with its copy never flushed. Seen from RAM on
    the unit (2026-11-15) with a stub that makes the same state (flat 1:1
    sections, RAM and flash window cached, MMU and both caches on, copy
    through the data cache): a 5644-octet loader, not flushed, and a
    5664-octet one, flushed, both start. The ARM926 does not fill a cache
    line on a write miss, which is why the unflushed copy is in RAM anyway;
    the loader is padded to whole lines so that it does not depend on that.
  - There is no second try: once a set is chosen and its core image passes
    bootm's CRC, a hang after the jump is final until power is cycled, and
    the next start makes the same choice. Fallback exists only for partitions
    that fail magic or checksum.
  - The built-in environment has `bootcmd=gsboot` and `bootdelay=1`. With an
    erased `uboot_env` U-Boot warns about the CRC and uses the built-in one,
    so a unit that never saved an environment runs `gsboot` by itself.
- `test` with a variable that does not exist (seen on the unit 2026-11-15):
  with no `v90d_tries` in the saved environment at all, the branch
  `if test "${v90d_tries}" = 1` of our trial script was taken: the box
  started our image and the saved environment then held `v90d_tries=0`.
  The empty word seems to be dropped and `test = 1` comes out true. A
  value that matches no branch (`0`, `x`) leads to `gsboot` as intended. So
  a script for this U-Boot must not rely on an unset variable comparing
  unequal.

## Kernel / board support
- Stock: Linux 3.4.20-rt31 (gcc 4.6.4), uClibc, busybox 1.15.3 (gsbusybox).
- **GPL source is public and complete** for this SoC: `ht8xx_gpl.tar.gz`
  (linux-3.4.20 + U-Boot) contains full `arch/arm/mach-dvf` support: clock,
  `dspg-uart`, `dspg-qspic`, `gpio-dspg`, `spi-dspg-dcx`, `sound/soc/dvf`
  TDM/PCM, CSS/coma, and per-board device trees (`ht802_9610003703a..713b`;
  ours, …725A, is newer than the 2017 tree but the SoC support is the same).
  No mainline/OpenWrt support exists; this GPL tree is the board-support base.

## Firmware image format (for our build system / recovery)
- Container: 648-byte header (magic, per-file names/sizes/versions/masks) + each
  sub-file as 512-byte header (first 32 B AES-encrypted) + AES-CBC body. Multi-
  image: carries both DVF9918 and DVF9919 `boot`/`core`; `base` and `prog` are
  single images shared across both SoC variants. The flasher `/bin/new_provision`
  verifies magic + checksum + version/generation/hardware-mask only: **no
  public-key signature** (it links OpenSSL but imports only X509 cert-parse +
  MD5; "RSA+SHA256" in the status is the device TLS/provisioning cert, not image
  signing). A repacked same-version image therefore installs; an older version
  is refused on the version/generation check.
- **Layout in full (2026-11-15)**, read from `new_provision` of 1.0.65 by
  disassembly and proven by `fw/tools/mkgsfw.py`, which opens and verifies
  the genuine 1.0.65.1 file and whose output for `base` equals the flash
  backup octet for octet outside the counter field. All numbers little endian.
  - Container directory, 0x288 octets: magic 0x23C97AF9 at 0; seven file
    names of 64 octets from 0x004; seven sizes (u32, each including the
    file's own 512-octet header) from 0x1C4; seven versions (u32, one octet
    per number, so 1.0.65.1 is 0x01004101) from 0x1E0; four u16 hardware
    bit words at 0x1FC..0x202; u16 mask at 0x204; u16 partition generation
    at 0x206. The files follow in directory order with no padding. One
    firmware file may hold several containers one after the other
    (1.0.65.1 holds a generation 1 and a generation 2 one).
  - File header, 512 octets, the same header that ends up at the start of
    the flash partition: 0x00 magic; 0x04 version; 0x08 size of the whole
    file; 0x0C `SizeUsed` (payload length); 0x10 u16 image id (0x0B boot,
    0x0C core, 0x0D base, 0x0E prog); 0x12 u16 checksum; 0x14 u16 year,
    0x16 day, 0x17 month, 0x18 minute, 0x19 hour; 0x1A u16 model key
    (0xFD21 on HT802 images); 0x1C u16 mask (1); 0x1E..0x24 four u16
    hardware bit words; 0x28, 0x2C compatible versions (in `prog`: the core
    and base it belongs to); 0x30 provision counter (0 in the file, filled
    in at install); 0x34 cpu text ("dvf9918", "dvf9919" or "any"); 0x54
    u16 partition generation.
    Correction to earlier notes: the stock 1.0.65.1 date fields read
    2025-12-10, not 2025-10-12 (day comes before month).
  - Checksum: the payload taken as 16-bit words plus the header field sums
    to 0 mod 65536. The header itself is not covered.
  - Cipher: AES-128-CBC over independent units of 32 octets, each starting
    again from the same fixed 16-octet start vector. Only the first 32
    octets of the file header are enciphered, with a fixed key; the payload
    is enciphered with a second key, which is octets 16..31 of the clear
    header with each pair of octets swapped. The fixed key is the result
    of the upgrader's own text to binary conversion of a 32-character
    constant, which treats lowercase letters wrongly; `mkgsfw.py` keeps the
    constant and the conversion in one commented place. This is obscurity,
    not signing: there is no secret a third party lacks.
- **The stock upload, as a browser does it** (1.0.65, seen 2026-11-15):
  Maintenance, Upgrade and Provisioning, "Upload Firmware: Upload from
  local directory" opens `/cgi-bin/firmware_page`, whose form posts
  `multipart/form-data` with one file field `firmware_file` to
  `/cgi-bin/upload_firmware?session_token=T`, T being the token the pages
  carry after login. The web server (mini_httpd) is up about 35 s after the
  shell answers.
- **What the stock upgrade does with an uploaded file** (web page
  `upload_firmware`, limit about 25 MB): a helper picks from the upload the
  first container whose first file name contains the model name and whose
  generation matches the unit's (0 counts as 1), stores it in /tmp and
  starts the upgrader on it, not forced. The upgrader then:
  1. checks the directory: magic; file names contain the model name
     (case ignored); generation equal to the unit's; the unit's required
     hardware bits (`/proc/gxp/dev_info/required_bits`) all present in the
     file's bit words; mask agrees with the headers already in flash.
  2. decides per file whether to install it. Targets are always the
     partitions of the set that is NOT running. A file is left out only when
     its version equals both the running and the other set's version; if no
     file at all differs from the running versions, nothing is done.
     **There is no "newer than" rule**: any different version installs. An
     image the container does not carry is skipped with a warning (so a
     container without `boot` leaves U-Boot alone).
  3. per file, in directory order: checks the file header (magic; cpu text
     empty, equal to the unit's or "any"; generation; version, size, bits
     and mask equal to the directory; required bits; model key, when not 0,
     equal to the one in the target partition's present header), deciphers,
     checks the checksum, sets the counter to one more than the highest
     counter of the running set, erases the whole target partition and
     writes header and payload from offset 0. Each file is in flash before
     the next is looked at.
  4. on success asks for a reboot. Which set starts is then decided by
     `gsboot` alone (see Bootloader); the upgrader keeps no flag for it.
  Earlier line corrected: an older release is refused by the generation
  (1.0.11.6 carries generation 0, the unit is generation 1) and probably the
  hardware bits, not by a version comparison.
- Hardware bit words over time (fourth word): 1.0.11.6 0x000F, 1.0.41.2
  0x3FFF, 1.0.65.1 0x7FFF. Inferred, not shown: one bit per board revision,
  set in every release that supports it. This unit's required bits have not
  been read yet (one `cat` on the stock system).
- A weakness of the stock procedure itself, by reading and by model, never
  provoked: power lost inside the first flash page of a file (magic already
  written, length field still erased) leaves a header that gsboot accepts
  as far as the magic and then sums over a length of 0xFFFFFFFF.
- The board part number is in `factory` as text at offset 0x20 (flash
  0x4020), NUL terminated: "9610003725A" on this board. Our loader uses it
  as its hardware check. The rest of `factory` holds per-unit data and is
  not described here on purpose.

## The HT801 (public flash dumps, the stock 1.0.65.3 file, one owner's unit)
- Two HT801 dumps (crvallance/Grandstream-hax; a raw chip read has each
  32-bit word byte-swapped against what the CPU sees): part numbers
  9611003610A (V1.0A) and 9610003611C (V1.1C). Same partition map as above.
- Device tree at flash 0x8000: both identical, and equal to this HT802's
  except for the second SLIC (one entry in `current_cs_gpio` and `int-gpio`,
  no second SPI slave with its reset line).
- Factory records (from factory 0x56 on: tag, count of 16-bit words, value,
  all little-endian):
  tags 0x0BC9 to 0x0BD3 hold the SLIC description in the order of the
  `hw_features` names. HT802: 1,0,2,0,1,2,1,0,0,0,3. HT801: the same with
  0x0BCE (number of SLIC devices) 1 instead of 2 and 0x0BCF 2 instead of 1.
  Converter type (0x0BCB = 2) and protection (0x0BCD = 1) are equal. Tag
  0x03E8 is 9 on the HT802 and 6 on both HT801s; meaning not known.
  No other record differs between this HT802 and the V1.1C HT801 besides
  the model name, so nothing in the factory data sets the Ethernet apart.
- Not readable from a dump: the SLIC's revision and which Ethernet PHY is
  fitted (the device tree says RMII with a fixed link on all three; the
  stock kernel reads the PHY's identity at run time).
- Stock firmware file `ht801fw.bin` 1.0.65.3: two containers, generation 1
  and generation 2, all names starting `ht801`. Generation 1 has the HT802's
  partition sizes (boot 0x40000, core 0x140000, base 0x300000, prog
  0x33F000); generation 2 is another flash layout with larger partitions
  (boot 0x80000, core 0x280000, base and prog 0x500000 each). Every file
  header carries model key **0xFD20** (the HT802's files 0xFD21) and
  hardware bits 0000:0000:0000:**FFFF** (the HT802 1.0.65.1 files 7FFF).
  Boot and core come for both processors (cpu text `dvf9918` and, in files
  named `*.dvf9919.bin`, `dvf9919`); base and prog say `any`.
- The upload page of 1.0.65.3 checks the file's name in the browser before
  anything is sent: the chosen file's base name must equal the value
  `firmware_filename`, which the page's script sets to the unit's
  `/proc/gxp/dev_info/dev_id` in lower case without dashes, followed by
  `fw.bin` (`ht801fw.bin`; the HT812/HT814 script uses `ht81xfw.bin`).
  Any other name gets "Your firmware file is invalid, please use correct
  firmware!" and no upload: nothing reaches the box, and its syslog shows
  nothing of it.
- One owner's V1.0A unit: part number **9610003610A** on the status page
  (one digit away from the dump's 9611003610A), a 25Q128 flash chip (16 MiB,
  so generation 1), a DVF9918, 32 MB of Etron SDRAM. Under stock 1.0.65.3 it
  reports 23740 kB of memory in total.

## RECOVERY (tested)
- **Full 16 MiB flash backup taken and verified**: all 15 partitions md5-match
  the box, in `~/v90d/recon/backup/` (`flash_all.bin` + `mtd_<name>.bin`,
  sha256 recorded). `mtd_prog2.bin` is the clean stock /app; `mtd_base.bin`,
  `mtd_uboot.bin`, `mtd_factory.bin` are the stock rootfs / bootloader / DTB.
- The device reflashes through its own validated web upload / restricted-shell
  `upgrade` (both drive `new_provision`), demonstrated working. UART + external
  SPI programmer remain the ultimate backstops (not yet wired up).

## The HT801 V1.1A, from a real unit (2026-11-24)

A first physical HT801 (hardware V1.1A, part 9610003611A) was reconned the
HT802 way: a root shell on the stock firmware from a same-version prog
repack (see "Root access" below, built from this unit's own 1.0.65.3
firmware), then a RAM boot of our system. Facts measured on the unit:

- SoC ARM926EJ-S rev 5 (ARMv5TEJ), `Hardware: DSPG DVF99`, the DVF9918 class
  as expected. `/proc/mtd` is this HT802's map exactly: 15 partitions,
  bootastic 16k, factory 48k, nvram 64k, uboot2 256k, core2/core 1280k,
  base2/base 3072k, prog2/prog 3324k, lang/lang2/oem 128k, uboot_env 8k,
  uboot rest. Stock boots set B (`bootpartition=1`, root `/dev/mtdblock7`).
- Flash: Winbond W25Q128FV, JEDEC id 0xEF4018 on CS0 (stock log
  `CS0: jedecid: 0xef4018`, run at 33 then 50 MHz). The id is already in our
  table (`refs/qspi-chips.md`): the board is not an unknown chip.
- Ethernet PHY: Micrel/Microchip KSZ8081, PHY id 0x00221561 at MDIO address
  0 (stock `eth0: PHY ID 00221561 at 0`), over stmmac/DWMAC1000; link
  100/Full. This is the V1.x board PHY BOARDS.md anticipated, and our kernel
  drives it with the generic PHY path.
- SLIC: one Si3217x on channel 0; the second chip select has no SLIC.
- Model key 0xFD20 and container hardware bits 0xFFFF (from this unit's own
  stock firmware, Release_HT801_1.0.65.3). These differ from the HT802
  (0xFD21 / 0x7FFF): an install image for an HT801 must carry 0xFD20 /
  0xFFFF, or the stock upgrader refuses it on the model-key check.
- Part number 9610003611A and model HT801 are ASCII in the factory
  partition (mtd1), but not at offset 0x4020 on this unit (that region reads
  empty); they sit further in.

RAM boot (`fw/tools/ramboot.sh`, writes nothing; RAMBOOT_ME = the
workstation, our telnet on 23): our Linux 6.6.63 came up and ran steadily
past the 100 s watchdog window, Ethernet up by DHCP on eth0 plus the
169.254.90.1 rescue, generic PHY link 100/Full. The line driver found one
port and brought it up without the vendor patch: `port 1: SLIC id b2 up,
battery 58612 mV, mu-law`; `port 2: no SLIC answers, this is a one-port
board`. The daemon started on /dev/v90line and the web interface served. A
power cycle returns the unit to stock.

Flash install (2026-11-24): with the per-model release (#17), whose HT801
file carries model key 0xFD20 and bits 0xFFFF, the stock upgrade page took
`v90d-ht801fw.bin` (uploaded as `ht801fw.bin`) and wrote it, and the unit
came up on v90d from flash: started by the loader from set A, board known,
start confirmed, Ethernet, web and the one FXS port up; the stock set
stays intact in set B. Our QSPI code drives this Winbond chip as it is (its
id is in the table since commit dd84708). An earlier RAM boot that reported
`flash not recognised` had used a kernel image built before that commit,
not a fault in the driver. The first install attempt failed only on the
model key (the HT802's 0xFD21), which the stock upgrader refuses.

## RECOVERY (tested)
- **Full 16 MiB flash backup taken and verified**: all 15 partitions md5-match
  the box, in `~/v90d/recon/backup/` (`flash_all.bin` + `mtd_<name>.bin`,
  sha256 recorded). `mtd_prog2.bin` is the clean stock /app; `mtd_base.bin`,
  `mtd_uboot.bin`, `mtd_factory.bin` are the stock rootfs / bootloader / DTB.
- The device reflashes through its own validated web upload / restricted-shell
  `upgrade` (both drive `new_provision`), demonstrated working. UART + external
  SPI programmer remain the ultimate backstops (not yet wired up).

## Root access (recon tool, temporary)
- gssu is removed from all firmware ≥ 1.0.19.x; downgrade to a gssu build is
  refused by the version check. Root was obtained instead by injecting a line
  into `/app/bin/ht_start.sh` (prog only; base untouched) that starts
  `busybox telnetd -l /bin/sh -p 2323`, firewalled to the workstation, and
  flashing that same-version repack via the validated web upload. `telnet
  192.168.1.172 2323` → root (uid 0). To be removed before any production image.
- Same route proven on the HT801 V1.1A (2026-11-24): its own stock
  Release_HT801_1.0.65.3 was taken, only the prog partition changed (the
  telnetd line injected into `/app/bin/ht_start.sh`, firewalled to the
  workstation 10.42.90.1), prog bumped 1.0.65.3 -> 1.0.65.4 so the upgrader
  installs it, the rest of the container kept byte for byte (so the HT801
  model key 0xFD20 and bits 0xFFFF are preserved), rebuilt with the ht801
  partition names and uploaded as `ht801fw.bin`. `telnet 10.42.90.143 2323`
  -> root (uid 0). The injected prog stays in the stock set (set B) of that unit, now that
  v90d is installed in set A.
