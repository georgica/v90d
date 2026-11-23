# OBi200: SPI controller and flash, watchdog, LEDs

Reconstructed from the stock Linux 2.6.30 kernel (RAM dump `ktext.bin`, its
`/proc/kallsyms`): the SPI driver `ffxav_spi_*` (c0246034..c0247524), its
platform device and board info (`ffxav_spi_init` c000c310, platform device
c04b0820, `ffxav_spi_board_info` c0024280), the patched `m25p80_read`
(c0245588), the GPIO driver `ffxav_gpio_*` (c00326ac..c0032a50,
`ffxav_gpio_init` c000c614); and the modules `ffxav_led.ko`,
`ffxav_wdt.ko`, `ffxav_mp.ko` (objdump -dr; offsets below are `.text` /
`.init.text` of the module). The dump is of a running system, so data
structures hold live values.

Marks: **[V]** read off the disassembly or the live data, **[I]** inferred
from how the code uses it, **[G]** guess. Nothing here has been checked on
hardware with our kernel yet.

Drivers:

| what | file | DT compatible |
|---|---|---|
| SPI controller | `drivers/spi/spi-ffxav.c` | `marvell,ffxav-spi` |
| flash | mainline spi-nor | `jedec,spi-nor` |
| watchdog | `drivers/watchdog/ffxav_wdt.c` | `marvell,ffxav-wdt` |
| LEDs on switch PHYs | `drivers/leds/leds-ffxav-phy.c` | `marvell,ffxav-phy-leds` |
| power LED (GPIO 5) | mainline `leds-gpio` on `drivers/gpio/gpio-ffxav.c` (the telephony agent's) | `gpio-leds` |
| tick feeding hand-over | `drivers/clocksource/timer-ffxav.c`, `include/linux/soc/marvell/ffxav.h` | |

## SPI controller

### Resources [V]

| what | value | source |
|---|---|---|
| registers | 0x8000c800-0x8000c8ff ("ffxav-spi.0") | resources c04b0b14 |
| IRQ | 23 (not used by the stock driver: no request_irq) | c04b0b30 |
| chip selects | 4 (platform data `{u16 num_chipselect = 4}` c04b0b10) | |
| clock | 100 MHz: `BAUDR = 100000000 / max_speed_hz`, rounded up to even, at least 2 (`ffxav_spi_setup` c0246728) | |
| system register | 0x8000f0ac: bit 27 cleared, then bit 26 set, then udelay(125), on every `setup` (c0246688) | |

The bits of 0x8000f0ac: meaning unknown [G: a reset or clock enable of the
SPI block or the SLIC, or the SPI pins' function; the telephony notes guess
pin function]. `mvSPIInit` in ffxav_mp.ko does the same with delays of 2.5
or 25 ms [V 0x69f4], which hints at a reset [G]. We do it once at probe
(second `reg` of the SPI node, mapped without requesting it).

### It is a DesignWare APB SSI [V layout, I identity]

Offsets from 0x8000c800 (the stock uses the fixed mapping 0xfcc0c800):

| off | name | use in the stock driver |
|---|---|---|
| 0x00 | CTRLR0 | `(chip.ctrl & 0xfcff) \| tmod`; chip.ctrl = 0x3c7 from controller_data with `mode << 6` merged (bit 6 CPHA, bit 7 CPOL); `\| 0xf` for 16-bit words. bits 3:0 frame size - 1 |
| 0x04 | CTRLR1 | NDF: frames to read - 1 in EEPROM read mode, else 0 |
| 0x08 | SSIENR | 1 enable / 0 disable (`ffxav_spi_enable/disable`) |
| 0x10 | SER | `1 << (chip_select - 1)`: the stock numbers chip selects from 1 |
| 0x14 | BAUDR | divider (see above) |
| 0x18 / 0x1c | TXFTLR / RXFTLR | 8 / 8, only in `mvSPIInit` (ffxav_mp.ko) |
| 0x20 | TXFLR | `16 - TXFLR` = room: the FIFO is 16 deep [V] |
| 0x28 | SR | bit 0 busy (polled before reconfiguring), 1 TX not full, 2 TX empty, 3 RX not empty |
| 0x2c | IMR | 0 (written in setup): no interrupts |
| 0x60 | DR | data, 32-bit accesses |

The other DesignWare registers (RISR 0x34, ICR 0x48, IDR 0x58, VERSION
0x5c) are not used by the stock; we read them for the probe dump and use
RISR/ICR for the receive-overrun check [I].

`ffxav_spi_cs_active/deactive` are empty: the chip select is the
controller's own [V]. On a DesignWare SSI that means: asserted while a SER
bit is set and the TX FIFO has data, released when the FIFO runs dry (for
mode 3; in modes 0/2 it is pulsed between frames) [I from the IP].

### How the stock moves data [V]

Modelled on bfin5xx_spi. `transfer` queues; a work item and a tasklet pump.
- Mode (TMOD, CTRLR0 9:8) is **1, transmit only**, for every transfer,
  except: when the *next* transfer of the message is receive-only
  (`rx_buf && !tx_buf`), `restore_state` sets **3, EEPROM read**, with
  NDF = next->len - 1 (or len/2 - 1 for 16-bit words). The current
  (transmit) transfer's bytes go out, then the controller clocks in NDF+1
  frames by itself, within the same chip select. Mode 0 (full duplex) is
  never used.
- Writers (`u8_writer` c0246b80, `u16_writer`) disable interrupts and keep
  the FIFO topped up (16 - TXFLR), then wait for TX empty.
- Readers poll RX not empty and read DR; reads are not protected against
  interrupts, so the stock's **m25p80 is patched to read at most 32 bytes
  per message, in 16-bit words** (c0245694: `len > 32 ? 32 : len & ~1`,
  command and address packed into two 16-bit words), i.e. never more than
  16 frames, the FIFO depth, can pile up.

### Ours

`transfer_one_message`, polled. A message is cut into chip-select groups
(transfers without `cs_change`, same speed and word size, no delays);
each group is one FIFO stream with interrupts off: the FIFO is filled with
SER clear, SER is set (the transfer starts), the FIFO is kept fed and the RX
FIFO drained. Mode per group: all transmit-only -> TMOD 1; transmit-only
transfers then receive-only ones -> TMOD 3 (EEPROM read), the stock's
case; anything else -> TMOD 0 with at most 16 frames in flight. Errors
(chip select lost because the FIFO ran dry, receive overrun, timeout) are
reported and fail the message. `cs_change` on the last transfer (keep the
chip select) cannot be done and is ignored. Transfers offered to spi-mem
users are limited to `ffxav_spi.max_xfer` bytes (256 by default, module
parameter, writable at run time) so that interrupts are never off for
long (256 bytes at 16.7 MHz: about 125 us). Word sizes 8 and 16, modes 0-3.
A simulation of the stream logic against a model of the controller passes
for the flash's read ID, 256-byte read, page program, and a 40-byte full
duplex transfer, whenever the bus is slower than one register access per
frame (the real ratio is about 1 to 10).

Probe prints all the controller's registers (`ctrlr0 ... version`), the
system register before and after, and the FIFO depth found the DesignWare
way (writing TXFTLR); the driver uses min(that, 16).

## The flash [V]

Board info (`ffxav_spi_board_info`, one entry): modalias `m25p80`,
`max_speed_hz` 20 MHz (actual 16.7 MHz: divider 6), bus 0, chip select 1
(SER bit 0), mode 3, platform data `flash_platform_data {name
"spi_flash0", parts c04b0ba8, nr_parts 7, type NULL}` (JEDEC probe),
controller_data `{ctrl 0x03c7, dma 0, bits 8, cs_change_per_word 0}`.

The partition table is in the kernel's data (not from U-Boot; the command
line is `console=ttyS0,115200 root=/dev/mtdblock5 rootfstype=squashfs
mem=56M brd=...`, no mtdparts) [V]. `struct mtd_partition` is 40 bytes
(64-bit size and offset):

| # | name | offset | size | end | mask_flags |
|---|---|---|---|---|---|
| 0 | u-boot | 0x000000 | 0x050000 | 0x050000 | 0 |
| 1 | kernel | 0x050000 (MTDPART_OFS_APPEND) | 0x280000 | 0x2d0000 | 0 |
| 2 | scratch | 0xb40000 | 0x0c0000 | 0xc00000 | 0 |
| 3 | rootfs | 0x480000 | 0x6c0000 | 0xb40000 | 0 |
| 4 | flash0 | 0x000000 | 0x1000000 | | 0 |
| 5 | obi app | 0xc00000 | 0x240000 | 0xe40000 | 0 |
| 6 | bluetooth | 0xf00000 | 0x100000 | 0x1000000 | 0 |

All writable on the stock. Not covered by any partition: 0x2d0000-0x480000
(the image header the firmware keeps at 0x470000 is there) and
0xe40000-0xf00000. On the stock these are mtd2..mtd8 (root=mtdblock5 =
rootfs, the whole flash mtd6), so two other MTD devices come first [I: the
"mtd-ram" strings next to the table].

Ours (DT `fixed-partitions`, in this order, so mtd0..mtd7):

| mtd | label | offset | size | |
|---|---|---|---|---|
| 0 | u-boot | 0x000000 | 0x040000 | **read-only** |
| 1 | factory | 0x040000 | 0x010000 | **read-only** ("OB100 UNIT INFO", MAC at 0x40100) |
| 2 | kernel | 0x050000 | 0x280000 | |
| 3 | rootfs | 0x480000 | 0x6c0000 | |
| 4 | scratch | 0xb40000 | 0x0c0000 | jffs2 /scratch |
| 5 | obi app | 0xc00000 | 0x240000 | /obi |
| 6 | bluetooth | 0xf00000 | 0x100000 | |
| 7 | flash | 0x000000 | 0x1000000 | **read-only** whole flash, for backups |

Bricking protection: the read-only partitions refuse write and erase in
the MTD core; the MTD master device is not exposed
(MTD_PARTITIONED_MASTER off); `MTD_SPI_NOR_SWP_KEEP` keeps spi-nor from
rewriting the status register (block protection) at probe; the flash is
16 MiB so no 4-byte address mode is entered; with a single-wire controller
no Quad Enable bit is ever written. What remains: a write to the writable
partitions, and any SPI client that addresses chip select 0 by mistake
(the SLIC must be `reg = <1>`).

`MTD_SPI_NOR_USE_4K_SECTORS` is on (the default): erase size 4 KiB if the
part has it. The stock 2.6.30 m25p80 also used 4 KiB sectors when the part
had them [I], which matters for the jffs2 on "scratch".

## The SLIC on the same bus [V]

From ffxav_mp.ko `init_module` (0x1c0..0x624), its own `spi_device` "mp_spi"
on bus 0: **chip select 2 (SER bit 1, our `reg = <1>`), mode 3, 8 bits per
word, max_speed_hz 2 MHz** (0x1e8480; divider 50, exactly 2 MHz),
controller_data the same `{0x03c7, 0, 8, 0}` (module .data+0x180), irq -1.
Accesses: `mvSPIReadByte` (0x6b0c) is one message, transmit n command bytes
(plus one 0 byte when the voice module type is 2 and n <= 2) then receive 1
byte, so on the stock an EEPROM read with NDF 0 [V + the controller's
logic]; `mvSPIWriteByte` (0x6c54) one transmit of n bytes (plus a 0 when
type 2 and n <= 3). `mvSPIInit` (0x69f4, exported, not called inside the
module) writes the controller directly (SSIENR 0, BAUDR, TXFTLR/RXFTLR 8)
and the 0x8000f0ac bits. The DT child is the telephony agent's
(`slic@1`, `silabs,si3226x-obi`).

With mode 3 a multi-byte message keeps the chip select low across the
bytes. If the SLIC wants the chip select released between bytes, send one
byte per transfer with `cs_change` (ffxav_tel's `spi_framing=1`): our
driver always releases the chip select between groups.

## GPIO [V]

0x8000c000, the DesignWare APB GPIO layout: 0x00 data out, 0x04 direction
(1 = output), 0x30 interrupt enable, 0x34 mask, 0x38 level/edge, 0x3c
polarity, 0x40 status, 0x4c EOI, 0x50 input. 16 pins (gpiochip c04b0e58,
base 0, ngpio 16); interrupts 32..47 demultiplexed from IRQ 12. Mainline
`snps,dw-apb-gpio` would drive it; the DT uses the telephony agent's
`marvell,ffxav-gpio` (one node for the block, label `gpio`). Users: SLIC
reset pin 0 and interrupt pin 2 (telephony), power LED pin 5.

0x8000f0dc bit 9: cleared by `ffxav_led.ko` (init 0x98..0xac) before it
requests GPIO 5, and by ffxav_mp.ko before pins 0 and 2 [V]; pin function
[I]. The telephony driver clears it; this region must not be requested by
anyone (each mapping is a plain ioremap).

## LEDs

### The stock LED module [V]

Char device "led" (dynamic major, 252 on the box). ioctl(cmd, ptr to
`{u8 led, a, b, pad; u32 count; u32 period}`): 0 all on, 1 on(led), 2
off(led), 3 state(led, a), 4 blink(led, a, b, count, period), 5 all off.
A kthread (`led_run`) runs the blinks.

`led_ctl` (bss), entry `i` at +0x14 + 96 i: 4 states of 16 bytes, then
`on` state index (+64), `off` index (+68), blink fields (+72..+92).
A state:

| off | what |
|---|---|
| +0 | u32: write a switch PHY register |
| +4, +5, +6 | u8 PHY, u8 register, u16 value (`gprtSetPhyReg(sw_qd_dev, phy, reg, value)`) |
| +8 | u32: set a GPIO |
| +12, +14 | u8 GPIO, u16 value |

Contents at init:

| LED | state 0 | state 1 | state 2 | state 3 | on / off |
|---|---|---|---|---|---|
| 0 power | PHY 4 r25 = 3, GPIO 5 = 1 | PHY 4 r25 = 3, GPIO 5 = 0 | PHY 4 r25 = 2, GPIO 5 = 1 | PHY 4 r25 = 2, GPIO 5 = 0 | 1 / 0 |
| 1 phone | PHY 2 r25 = 2 | PHY 2 r25 = 3 | - | - | 1 / 0 |
| 2 | PHY 3 r25 = 2 | PHY 3 r25 = 3 | - | - | 1 / 0 |

Also at init: PHY 0 register 22 = 0x4a4a; 0x8000f0dc bit 9 cleared;
`gpio_request(5, "led green")`, output 1 (off); LEDs 1 and 2 off; LED 0
blinking states 1/0 with period 100 for ever.

Readings:
- GPIO 5 is the green power LED, **active low** (on = state 1 = 0) [V].
- Register 25 of a switch PHY is the LED override register (fields of two
  bits per LED: 0 normal, 2 off, 3 on, as on Marvell PHYs) [I from the
  values: on = 3, off = 2 for the phone]; the value written only sets the
  LED0 field, the other LEDs of that PHY stay normal (link/activity) [I].
- PHY 2's LED0 = the phone LED [V that LED 1 uses it; that LED 1 is the
  phone is from the parent's notes].
- PHY 3's LED0 = the module's LED 2 [V]; a second phone LED on two-port
  models, perhaps not fitted on the OBi200 [G].
- PHY 4's LED0 is forced on in both the power LED's on and off states and
  off in states 2/3: a second colour of the power LED [G: red/amber; the
  label "obi200:red:power" is a guess].
- Register 22 of PHY 0 = 0x4a4a: LED function select [G].
- The PHY registers are memory mapped in the switch window
  (0x8000e000 + (phy << 7) + (reg << 2), ETH-NOTES.md), the QD library's
  PHY address = the number passed [V `gprtSetPhyReg` -> `hwWritePhyReg`,
  `portToSmiMapping` identity for PHYs [I]].

### Ours

- `gpio-leds`: "obi200:green:power", `<&gpio 5 GPIO_ACTIVE_LOW>`,
  trigger `default-on`.
- `marvell,ffxav-phy-leds` (reg 0x8000e000, 0x380, mapped without request:
  the Ethernet driver owns the switch window): writes PHY 0 register 22 =
  `marvell,led-select` (0x4a4a) at probe (prints old and new), then one LED
  per child, reg = PHY: "obi200:green:phone" (PHY 2),
  "obi200:green:phone-2" (PHY 3), "obi200:red:power" (PHY 4). Brightness
  writes 3 or 2 to register 25; reads back the LED0 field. Default state
  "keep": nothing is changed at probe; register 25 of each is printed.

Triggers in the config: timer, heartbeat, default-on.

## Watchdog

### Registers [V access, I meaning]

0x8000d800, DesignWare APB watchdog layout:

| off | name | |
|---|---|---|
| 0x00 | CTRL | stock writes 7: bit 0 enable, bit 1 interrupt on the first expiry (reset on the second), bit 2 reset pulse length [I from the IP] |
| 0x04 | PERIOD | stock 0xf4; 0.1 s x (n + 1) for the low nibble n, 0xff = 1.6 s (the parent's measurement) |
| 0x08 | counter | [I] read only in our dump |
| 0x0c | FEED | write 0x76 |
| 0x10 | status | [I] read only in our dump |
| 0x14 | ACK | read clears the interrupt |

IRQ 3 is the first expiry.

### The stock module [V]

`init_module`: char device "watchdog" (dynamic, 251 on the box),
`request_irq(3)`, then **0x8000f028 = 0xbfffffff** (old value saved, ioctl
4 returns it: probably the reset cause [G]), PERIOD = 0xf4 (0.5 s), CTRL =
7, mode 1. The interrupt handler feeds the dog (0x76 to 0x0c, read 0x14):
mode 1 always, mode 2 while a counter (in interrupts, i.e. half seconds) is
not zero, decrementing it. ioctls: 0 and 5 set the timeout (counter = 2 x
arg, 4 if 0) and mode 2 (5 also keeps mode 2 on close); 1 mode 1; 2 mode 0
("Let dog bark now", and leaves preemption off: reset); 3 keep alive
(reload); 4 the saved 0x8000f028. close: back to mode 1 unless 5 was used.
Nothing else (no stop of the hardware, no restart handler).

`ffxav.c` restart: 0x8000f028 bit 29 cleared, bit 28 set, CTRL &= ~3,
PERIOD 0, CTRL |= 1 [from the parent].

### Ours

Probe: dump CTRL, PERIOD, counter, status and 0x8000f028; 0x8000f028 =
0xbfffffff (as the stock; on the kjump path it is that already); PERIOD
0xff, CTRL 7, feed; register with the watchdog core: `max_hw_heartbeat_ms`
1600, so the core feeds every 0.8 s from its worker until user space opens
/dev/watchdog (handle_boot_enabled), and then as long as user space keeps
up with its own timeout (default 60 s, `timeout-sec` in DT or
WDIOC_SETTIMEOUT, any length). No stop op: closing (magic close) hands the
feeding back to the core. After registering it calls
`ffxav_timer_stop_feeding()`: the tick stops feeding. If the driver does
not bind, the tick keeps feeding for `ffxav_feed_s` seconds as before.
IRQ 3 is not requested.

## What to check first on hardware

1. Boot log: `ffxav-spi ...: ctrlr0 ...` (state the stock left), `system
   register ...0f0ac: x -> y` (y should equal x on the kjump path), `FIFO 16
   deep`. Then spi-nor: a line naming the part and "16384 KiB" (spi-nor
   reads the JEDEC ID, prints `spi-nor spi0.0: <name> (16384 Kbytes)`), and
   `8 fixed-partitions partitions found` with the table.
2. `cat /proc/mtd`; `hexdump -C -s 0 -n 32 /dev/mtd1` must show "OB100 UNIT
   INFO"; `hexdump -C -s 0x100 -n 16 /dev/mtd1` the MAC (9c:ad:ef:65:30:63
   on the bench unit). Compare `dd if=/dev/mtd7 bs=64k | md5sum` twice
   (stable reads), and against a dump made under the stock firmware if one
   exists.
3. `echo 1 > /dev/mtd0` style writes must fail (EROFS) on mtd0, mtd1, mtd7.
4. Read speed: `time dd if=/dev/mtd3 of=/dev/null bs=64k` (6.75 MiB);
   errors in the log ("transmit FIFO ran dry", "receive overrun") mean the
   CPU did not keep up: lower `ffxav_spi.max_xfer` (e.g. 16) or the flash's
   spi-max-frequency.
5. Watchdog: `ffxav-wdt ...: ctrl 00000007 period 000000f4 ... sys bfffffff`
   on the kjump path, then "running", and the timer's "the watchdog driver
   feeds the watchdog now". The box must stay up past `ffxav_feed_s`.
   `echo V > /dev/watchdog` style test: open, write once, stop writing ->
   reset after the timeout (60 s).
6. LEDs: `ls /sys/class/leds`; `echo 0 > .../obi200:green:power/brightness`
   turns the green power LED off; `obi200:green:phone` on/off; the PHY
   register dump at probe (`phy n register 25: ...`).

## Uncertainties, by risk

1. The EEPROM-read stream at 16.7 MHz with interrupts off and 256-byte
   transfers was never done by the stock (it read 32 bytes at a time). If
   the CPU cannot keep up, reads fail (reported, not silent); the fix is
   the max_xfer parameter or a lower frequency. [I]
2. Full-duplex mode (TMOD 0) was never used by the stock; only reached by a
   message that both sends and receives in one transfer (not the flash,
   not the SLIC in its stock framing). [I]
3. 0x8000f0ac bits 26/27 and 0x8000f028 = 0xbfffffff are replays of the
   stock without knowing their meaning; on the kjump path they should not
   change anything (the dumps show it). [G]
4. PHY LED meanings (which LED is which, colours, register 22). Default
   state "keep" so nothing changes until someone writes brightness. [G]
5. 0x8000f0dc bit 9 for GPIO 5 depends on the telephony driver (or on the
   stock having run before kjump). [I]
