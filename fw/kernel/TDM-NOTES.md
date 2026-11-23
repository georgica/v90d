# OBi200 telephony on our kernel: TDM, SLIC, /dev/fxsN, /dev/pcm

Reverse-engineered from the stock Linux 2.6.30 image (dumped from RAM) and
its telephony module `ffxav_mp.ko` (relocatable, not stripped; built from
Marvell's "mv_phone" telephony driver for the FFxAV, author string Eran
Ben-Avi). No source of either exists. Driver: `drivers/misc/ffxav_tel.c`
(TDM block, SLIC on SPI, the character devices) and
`drivers/gpio/gpio-ffxav.c` (the pins, for the SLIC reset).

Tags: **[V]** verified in the code (instruction addresses given where it
matters; module offsets are `.text` of ffxav_mp.ko, kernel addresses are
virtual 0xc0xxxxxx of the stock image), **[I]** inferred, **[G]** guessed.
Nothing here has been checked on hardware with our kernel yet.

## The audio block (port 0 of "ffxav-audio", 0x80018000)

The module does not touch the block's registers itself: it calls the
kernel's built-in Marvell audio library (`mvAudioIntf*`, `audio*`,
c02ae018..c02b97c8) [V]. Its sequence, at module load (`starthw` = 1 in the
module's .data, so `mv_ffxav_tdm_probe` starts the highway at once and
nothing ever stops it; hence IRQ 24 firing all the time) [V]:

    port = mvAudioIntfOpenPort({port 0, TX, 0,0,0, 2, 1, 1, 0, 1, ops, 0})
    mvAudioIntfSetHwConfig(port, {10, 8, 32, 8000, 1})
    mvAudioIntfSetupDma(port, 0xa000, 16, 16, 80)
    ... the same for RX ...
    memset both 0xa000 buffers to 0; mvAudioIntfStart(TX, 0); Start(RX, 0)

`{10, 8, 32, 8000, 1}` = TDM mode 10, 8 slots per frame, 32-bit samples
(slots), 8000 Hz, and 1 (TDM_CTRL bit 18, [G] clock/frame master).
`(0xa000, 16, 16, 80)` = ring bytes, interrupt periods, descriptors, frames
per descriptor. The HPT/PTP, threshold and timer (IRQ 6) paths are not
taken for these arguments [V]: IRQ 6 stays silent [I].

Addresses physical. The stock kernel maps 0x80000000 at virtual 0xfcc00000.

### Registers (port 0; port p is +p*0x400)

| address | name [G] | as used |
| --- | --- | --- |
| 0x80018000 | PORT_CTRL | bit 1 HPT TX start (cleared), bit 3 shared port (cleared), bit 8 "volume on" (set at open) [V] |
| 0x80018004 | PORT_CLK | [7:0] rate code (8000 Hz = 0x43, table c03c6f60) [V]; [10:8] bit clock multiplier, [18:16] master clock multiplier (64->0, 128->1, 256->2 ...) [V]; bit 24 PLL locked (polled) [V]; bits 31:29 an update sequence [V] |
| 0x80018008 | INT_CAUSE | write 1 to clear [V] |
| 0x8001800c | INT_MASK | TX causes 0x70f05, RX causes 0x30000a [V]; each bit's meaning [G] |
| 0x80018010 | CHAN_CTRL | [5:0] TX slots = 8; bit 14 TX running; bit 15 TX enable; [21:16] RX slots = 8; bit 30 RX running; bit 31 RX enable [V] |
| 0x80018040 | HPT calibration | written 0 [V] |
| 0x80018108 / 0x80018208 | TX / RX TDM_FORMAT | 0x7a5: [2:0] sample size code 5 (32 bit), [7:5] slot width code 5, [14:8] slots-1 = 7 [V] |
| 0x8001810c / 0x8001820c | TX / RX TDM_CTRL | bit 0 "start", **never written, preserved** [V]; bit 1 reset pulse, bit 2 FIFO flush pulse; [15:4] frame length-1 = 255 (256 bit clocks); [27:20] frame sync width-1 = 0; bit 17 set, bit 16 clear (mode 10) [G: sync timing]; bit 18 = 1; bit 31 held set while written [V]; kept bits 0xf008000f |
| 0x8001b400 | ID | version 1, 2 ports [D] |
| 0x8001b408 | bit 0 "PTP time good", set at init [V] |
| 0x8001b410 / 414 | global cause / mask | bit 24 TX DMA, bit 25 RX DMA, bit 29 port error, bit 31 ?; init enables 0xa3000000 [V] |
| 0x8001b418 / 41c | port error cause / mask, bit p [V] |
| 0x8001b438 / 43c | DMA done cause (W1C) / mask: bit 0 TX port 0, bit 16 RX port 0 [V] |
| 0x8001b500 | clock route port 0: [2:0] TX source, bit 7 latch, [10:8] RX source, bit 15 latch; bit 31 soft reset, self-clearing [V] |
| 0x8001b800 | DMA_CFG = 0x80000e70 [V] |
| 0x8001b804 | ring valid bitmask [V] |
| 0x8001b80c | ring enable command: write 0x82100000, poll bit 31, read the enabled mask, write it back \| 0x83000000 with the ring's bit set/cleared, poll [V]; 0x83100000 = all off [V] |
| 0x8001b880 + 0x20*r | ring r (0 = TX, 1 = RX of port 0): +0 CFG = (old & 0x83f) \| 0xa00<<16 \| rx<<15 \| 0x3000; +4 PTR [10:0] producer (write), [26:16] hardware index; +8 DONE [10:0] completed, write 0x8000\|n to take n; +0xc descriptor base; +0x10 threshold [V] |
| 0x8000f0ac | audio clocks: boot toggles bits 13-14 (strap 0x8000f094 bit 4), 9-10, 11-12, 7-8, then sets bit 21, kicks 0x8000f0b0 bit 4 and polls 0x8000f0a8 bit 16 [V c02b42dc] |
| 0x8000f0dc | bit 2 set by each port start [V] ([G] pads of the TDM port) |

### DMA [V unless marked]

- Descriptor: 32 octets, w0 = length in words (0x280) \| bit 21 (data
  format; from cfg[7]=1, [I] octet stream, MSB first) \| bit 22 (HW time, TX);
  w1 time; w2 buffer physical address; w3 TX channel-replace mask; w4
  status; w5 the buffer's virtual address (a cookie). No ownership bit:
  ownership is the PTR/DONE indices.
- 16 descriptors per ring, set up once; descriptor i = buffer + i*0xa00.
  A frame is 32 octets (8 slots x 32 bits); a descriptor is 80 frames =
  10 ms; the ring 160 ms. `dma_alloc_coherent`, not the SRAM.
- TX: 4 descriptors in flight. Start writes PTR = 1, 2, 3, 4. Each TX-done
  interrupt: DONE n; per completion write DONE = 0x8000\|1, re-attach the
  next descriptor and PTR = (PTR+1) & 15. The library then calls the module
  once per frame; the module writes 0x4600 octets ahead, which works out as
  **descriptor c+1 after the c-th completion** (the next one to play).
- RX: all 16 descriptors attached with PTR 0 and the ring enabled. Each
  RX-done interrupt: n = DONE (max 4); PTR += n (mod 16); DONE = 0x8000\|n;
  then the module is called for each frame of the descriptors that came in.
- Interrupt 24: one TX and one RX completion every 10 ms each, so ~200/s
  (the "every 5 ms" seen) [I]. The library masks the source in 0x8001b43c
  in the top half and handles it in a tasklet. Port error (global bit 29):
  it masks both DMA interrupts of the port, calls the module's error op
  (printk "TX error"/"RX error"), stops and resets the port, and nothing
  restarts it [V code, I effect]: that is the real "channel stops for good".

### Clocking and slots

The port's PLL in the 8 kHz family, master clock = bit clock = 256 fs =
2.048 MHz; a frame is 256 bit clocks (8 slots of 32 bits); frame sync one
bit clock wide, once a frame; the block makes the clocks itself, so the
FFxAV is the PCM master [I]. The module reads channel 0 at octets 0 (MSB)
and 1 (LSB) of a frame and channel 1 at octets 10 and 11 [V
mv_ffxav_tdm_rx_callback 0x458, get_slot_byte 0x420], i.e. 8-bit time slot
addressing: frame octet k = bits 8k..8k+7 after the frame sync [I]. The
SLIC is programmed by user space (fw/drv/slic.c, obiline: PCM start 0 for
channel 0); the module's own table (`time_slot_info_2chan`, .data+0x1f8)
gives start counts 0 and 10 octets = 80 bit clocks for channel 1 [V].

### Our sequence (ffxav_tel.c)

Probe (cold path, rings not running; not tried on the box, and its ring
enable command does not answer there, see below): dump the registers ("as
found"); allocate both 0xa000 rings and descriptor tables; `audio_init` (all rings off, DMA_CFG, clock route of
port 0, the 0x8000f0ac audio clocks **only if not already set** (they are
after a RAM boot from the stock system), global mask 0xa3000000, PTP bit);
request IRQ 24; `audio_start` = the stock OpenPort/SetHwConfig/SetupDma/
Start for TX then RX, register for register; dump again ("started"). The
highway runs from then on, as on the stock system. The interrupt handler
does the tasklets' work in hard IRQ context: TX: per completion ack,
PTR+1, fill descriptor (played + `tx_lead`) from the next queued block
(silence if none); RX: copy the n descriptors that came in, PTR += n,
DONE = 0x8000|n. Port errors are acked and counted, nothing is stopped.
Module parameters: `tx_lead` (1, the stock's; up to 3), `chan_off`
(0,10), `lsb_first` (n).

### On the box: the rings cannot be re-enabled, so they are adopted [V hw]

Results from the unit (RAM boot from the stock kernel), 2026-11-23:
- The ring **set** command (0x8001b80c = 0x83......) is never acknowledged
  (bit 31 stays), in any state tried, also under the stock kernel with its
  DMA running. The **query** 0x82100000 is acknowledged at once and reads
  0x02100003 (rings 0 and 1 enabled). [V hw]
- After the jump the stock rings stall (nobody advances the producer): rx
  cfg 0x0a00b030 ptr 0x80010002 done 0x00010000 base 0x030e0000 thr 0x0f;
  tx cfg 0x0a003030 ptr 0x80060006 done 0x00060000 base 0x030a0000;
  0x80018010 = 0xc008c008 (both channels enabled and running), 0x80018008 =
  3 (cause bits 0 and 1 pending), DMA_CFG 0x80000e70, valid 3. [V hw]
- Writing PTR works: rx PTR=10, tx PTR=14 -> within 30 ms rx 0x8009000a /
  done 0x00090008, tx 0x800e000e / done 0x000e0008: 8 descriptors each at
  once (stale descriptors), then stop. So PTR[10:0] is the producer,
  PTR[26:16] the hardware index, DONE[10:0] the completed count, and the
  hardware index stops at the producer (TX) or one before it (RX, one
  descriptor in progress) [I from these numbers]. The 8-at-once is not
  10 ms pacing: either the stale descriptors or the port's pending error
  state (cause 3) let the DMA run free [I].
- 0x03000000..0x031fffff is reserved (no-map, `stock_audio_dma`) in the
  tree so the stock rings and buffers cannot hit our kernel.

So ffxav_tel, when the query shows rings 0 and 1 enabled, **adopts** them
(`audio_adopt`) and only falls back to the cold path otherwise:
1. mask the DMA-done and port-error interrupts, request IRQ 24;
2. read each ring's base (+0xc), PTR and DONE (logged as "tx ring:" /
   "rx ring:"); the bases must lie in the tdm node's `memory-region` (the
   reserved region, mapped write-combined); our descriptors are written
   there, over the stock's, pointing at our own buffers
   (dma_alloc_coherent, zeroed): w0 = 0x00200280, w2 = buffer + i*0xa00;
3. take the completions found (DONE = 0x8000|n), clear the port causes and
   the DMA-done causes; leave 0x80018010 as found (the channels keep
   running), unless `ffxav_tel.adopt_restart=1` (then the stock Start's
   toggle: enables off, wait for the running bits, causes cleared, on);
4. TX: next to play = hardware index; producer = hw + 4 (four in flight,
   as the stock Start). RX: next to come = hardware index; producer =
   hw - 1 (fifteen free);
5. unmask (global 0xa3000000, port error, port causes, DMA done) and write
   the two producers last.
Guard: more than 300 completed descriptors in 0.5 s (100 expected) masks
the DMA interrupts and logs "DMA unpaced" with the ring and port registers
(`stats`: `unpaced 1`); 1000 port errors mask the port error interrupt.
Removing the driver only masks the DMA interrupts (the rings cannot be
turned off).

If the blocks come unpaced (completions at once instead of every 10 ms),
check in this order: the port causes (0x80018008: do they come back at
once after clearing? then the port is in error and only the stock
ErrorReset path, stop + soft reset 0x8001b500 bit 31 + start, may bring it
back; try `adopt_restart=1` first); the descriptor word 0 length (0x280
words = 2560 octets; the hardware might count octets or frames instead);
w0 bit 21; whether the hardware reads our descriptors at all (its DONE
advances but our RX buffers stay 0 and the stock buffers at 0x03180000
change: it cached the old ones); the stock descriptor contents as found
(dump 0x030a0000 and 0x030e0000, 16 x 32 octets, before our probe) to
compare word by word.

### Ring indices, status write-back, halts (second hardware round)

From the stock library [V addresses] and the unit's numbers [V hw]:
- **PTR bit 31**: no stock code writes it or reads it. Every PTR write is
  an 11-bit value (audioDmaRingTx c02b5294, refill c02b7130, RingEnable
  c02b46a8), every read masks [10:0] or [26:16] (AUDIO_RING_RX_DMA_FULL
  c02b43e0). Nothing in the library restarts a ring: the stock never lets
  one starve (TX keeps 4 queued, RX gets every descriptor back at once).
  Bit 31 set = halted/starved [I hw]. Writing the index has resumed a
  halted ring once (first round) and not another time (after our guard
  masked the interrupts); unexplained.
- **TX**: PTR[10:0] = producer; the hardware plays from its index up to
  the producer and stops when they meet (found: 0x800c000c).
- **RX**: PTR[10:0] = the *consumer*: the stock refill adds n to it as it
  takes n completions (c02b7110..c02b713c), so it trails the hardware index
  by DONE[10:0]; the ring is full at 15 (found: hw 7, PTR 8, DONE 15;
  running stock: PTR 0x00030003, DONE 0). Adoption now writes PTR = hw
  after taking all completions (it wrote hw - 1 before: harmless, one
  descriptor less).
- **Write-back**: on completion the hardware leaves status in w4, the time
  in w1 and w0 bit 22 (audioDmaRingRx c02b4cc0 and audioDmaRingTxDone
  c02b4ed0 read w4, w1, w0[13:0], w0 bit 22). The stock rewrites a
  descriptor before it gives it back: TX the whole descriptor
  (audioDmaRingTx c02b5204 -> audioDmaRingAttachBuffer c02b501c) before each
  PTR step; RX w5, w2 and w4 = 0 (audioDmaRingRxAckRefillQuick c02b4fc0)
  for every descriptor taken. Our first adoption build wrote the
  descriptors once and reused them as the hardware left them: the prime
  suspect for completions faster than the highway [I]. Now done as the
  stock does (`desc_fill`, `desc_refill_rx`).
- **Rate**: DONE counts descriptors (the library takes exactly one per
  audioDmaDecCompletionCounter(ring, 1) after one descriptor's worth,
  c02b4dc0, c02b4fb8), not frames or words [V].
- The pacing guard counts only the rings whose cause bit is set
  (coordinator's fix) and is off for the first second after adoption (the
  backlog of rings coming back from a halt).

### Hazard for the RAM boot (fw/obi/ramboot)

The stock highway is never stopped: `tdmoff` only turns the module's
channels off, the audio DMA keeps running. In the stock state dumped, the RX
ring writes 0x03180000..0x03189fff (and descriptor status at 0x030e0000)
every 160 ms, the TX ring reads 0x03140000.. and 0x030a0000 [D]. Those are
ordinary RAM of our kernel until ffxav_tel probes (its `audio_init` turns
every ring off first). Stop it before kjump, e.g. in ramboot.sh before the
jump: `"$here/pmem" -w 8001b80c 83100000` (all rings off; the stock's
interrupt then finds nothing) or better make the box's last step write
0x8001b80c = 0x83100000 and clear 0x80018010 bits 15 and 31 [I: from the
stop sequence; not tried].


## The SLIC on the SPI bus

The module does not bit-bang and does not touch the SPI controller's
registers on its data path: it is an ordinary 2.6.30 `spi_driver` ("mp_spi")
and creates its own `spi_device` on bus 0 [V init_module 0x1c0..0x624]:

| field | value |
| --- | --- |
| bus | 0 (`spi_busnum_to_master(0)`) [V] |
| chip select | **2** (`spi->chip_select = 2`, 0x1e4) [V]; the "SPI CS: 1" it prints is another variable [V] |
| mode | **3** (CPOL=1, CPHA=1; `spi->mode = 3`, 0x5f4) [V] |
| bits per word | **8** [V 0x600] |
| max_speed_hz | **2 000 000** (0x1e8480, 0x5f0) [V] |
| irq | -1 [V] |
| controller_data | pointer to `{u16 0x03c7, u8 0, u8 8, u8 0, ...}` (module .data+0x180) [V]; for the stock `ffxav_spi` (a DesignWare SSI driver modelled on bfin5xx_spi) that is the chip config: CTRLR0 0x03c7, no DMA, 8 bits, `cs_change_per_word` 0 [V ffxav_spi_setup c0246688] |

The SPI controller's own setup (`ffxav_spi_setup`) clears bit 27 and sets
bit 26 of 0x8000f0ac (pin function of the SPI pins, [I]) and derives the
clock divider as 100 MHz / max_speed_hz = 50 [V c0246728]. That belongs to
the SPI controller driver (another agent's), not here.

Module parameter `vm` (voice module type) is **2** in the module's .data
[V .data+0x264] and nothing on the box overrides it [I: obi launcher
loads it without arguments]. With vm=2:

- channel 0 = `/dev/fxs0`, CID octet bits 0x00; channel 1 = `/dev/fxs1`,
  CID 0x10 [V initSlic 0x68e8: `cid = ch ? 2+14 : 0`]
- **register read** (`readDirectReg` 0x3a4c, `mvSPIReadByte` 0x6b0c): one
  `spi_message`, CS held across it [V]:
  - transfer 1: tx only, 3 octets: `0x60|cid`, `reg`, `0x00`
    (the 0 is appended because vm==2 and the length was <= 2, 0x6b98)
  - transfer 2: rx only, 1 octet: the value
- **register write** (`writeDirectReg` 0x35b4, `mvSPIWriteByte` 0x6c54):
  one transfer, tx only, 4 octets: `0x20|cid`, `reg`, `value`, `0x00` [V]
- one mutex around every access (`spike_ctl.lock`) [V]
- the ioctl path disables the phone's IRQ (34 = GPIO 2) around itself [V];
  no IRQ is ever requested though (`request_irq` is not imported) [V]

Why the extra 0 octet is there is not known. The stock controller runs CTRLR0 0x03c7 = 8-bit frames, SCPH=SCPOL=1,
TMOD=3 (EEPROM read) [I from the DW SSI register layout]; in that mode a
tx transfer followed by an rx transfer becomes one "send N, then receive M"
operation with the chip select held. Whether the part sees the value in the
third or the fourth octet slot depends on that controller's behaviour [G].
Our driver therefore reproduces the module's messages octet for octet by
default and has a module parameter for the alternatives:

| `ffxav_tel.spi_framing=` | read | write |
| --- | --- | --- |
| 0 (default) | tx `{0x60\|cid, reg, 0}`, then rx 1 octet | tx `{0x20\|cid, reg, val, 0}` |
| 1 | three 1-octet transfers, CS released between them (`cs_change`), value from the third | three 1-octet transfers |
| 2 | one 3-octet full-duplex transfer, value = 3rd octet received | tx `{0x20\|cid, reg, val}` |

**RAM (indirect) registers:** no separate path. `fw/drv/proslic.c` reaches
the Si3226x RAM through direct registers 4 (RAMSTAT), 5 (address high), 6..9
(data) and 10 (address low), with ordinary register reads and writes; that
works through `FXS_REG_RD`/`FXS_REG_WR` unchanged [V proslic.c]. The
module's own `readIndirectReg`/`writeIndirectReg` are Si321x code (registers
28..33) never reached on this path [V].

## The SLIC reset line and the pins

The module's `mvBoardTdmGpioInit` (0x2fc4) [V]:
- clears bit 9 of **0x8000f0dc** (pin function, [I] so that pins 0 and 2 are
  GPIO); in our tree that bit belongs to the pinctrl-single node of the
  SPI/LED work (it clears it for the power LED); ffxav_tel does not touch it
- `gpio_request(0, "ffxav_mp reset")`, `gpio_request(2, "ffxav_mp intr")`,
  `gpio_direction_input(2)`

`mvBoardTdmMppSet` (0x2dd8, vm=2): `gpio_set_value(0, 0)`, mdelay(50),
`gpio_set_value(0, 1)` [V] (without making the pin an output: the board
comes up with the part in reset, the README's "held in reset from boot").
Ioctl 0x71b6: arg 0 -> `gpio_direction_output(0, 0)` "Reset SLIC chip";
otherwise `gpio_direction_output(0, 1)` "UnReset SLIC chip" [V 0x24e0,
0x2710]. So **GPIO 0, active low**, is the SLIC's RESET.

The GPIO block (stock `ffxav_gpio_*`, chip base 0, 16 pins) [V c00326ac..]:

| offset (0x8000c000 +) | use |
| --- | --- |
| 0x00 | output levels |
| 0x04 | direction, 1 = output (set after the level) |
| 0x30, 0x34, 0x38, 0x3c | interrupt enable, mask, edge/level type bits [I] |
| 0x40 | interrupt cause (read), written to 0x4c to ack [V gpio_irq_handler] |
| 0x4c | interrupt ack |
| 0x50 | input levels |

The GPIO interrupts are demultiplexed from interrupt controller line 12
(GPIO irqs 32..47) [V ffxav_gpio_init]. Not used by us.

## The ioctls of /dev/fxsN (mp_ioctl 0x2338)

All need `capable(CAP_SYS_ADMIN)`, else the module returns -1 (EPERM) [V].
The module accesses the argument as a kernel pointer without copy_*_user
[V]; we use get_user/put_user. Unknown commands: the module prints "mv_phone
...: unsupported IOCTL" and **returns 0** [V 0x248c]; ours return ENOTTY.

| cmd | module | ours |
| --- | --- | --- |
| 0xc00171b2 | read register `arg[0]` into `arg[4]` (SLIC: readDirectReg; DAA: other) | same |
| 0x400471b3 | write `arg[4]` to register `arg[0]` | same |
| 0x7191, 0x7192 | return 0, nothing else [V 0x2604] | same |
| 0x718a | `mvTdmChRxEnable(ch)`, `mvTdmChTxEnable(ch)` | channel on, both ways |
| 0x718b | `mvTdmChRxDisable`, `mvTdmChTxDisable` | channel off |
| 0x71b6 | SLIC reset line, above | same |
| 0x400171ad | write `arg & 0xff` to register 64 (linefeed of a Si321x) | ENOTTY |
| 0x40027186 | ring cadence | ENOTTY |
| 0x40047187 | activate ringing (register writes) | ENOTTY |
| 0x7188 | stop ringing | ENOTTY |
| 0x71a1..0x71a4 | busy / ring back / dial tone / stop tone | ENOTTY |
| 0x71ab | toggle bit 6 of register 72 | ENOTTY |
| 0x71ac | NTT CAR tone | ENOTTY |
| 0x71ae, 0x71af, 0x71b0, 0x71b1, 0x400471a4 | DAA (FXO) only | ENOTTY |
| 0x400471b5 | "not implemented yet", 0 | ENOTTY |
| 0x8004719a | hook event (reads register 68) | ENOTTY |
| 0xc00471b4 | returns the word at kernel address `arg \| 0xf1000000` | ENOTTY |

Open: one reader and one writer per device, else EBUSY ("device is busy
(read)") [V mp_open 0x27a4]; open also writes SLIC registers 21=0x10,
22=0x02, 23=0 (Si321x interrupt enables) and close writes 21..23=0 and
18..20=0xff, and resets the channel's TDM [V]. We only reset the channel's
TDM on close: the register writes are Si321x numbering, they hit a part held
in reset when obiline opens the devices, and obiline resets the part itself.

## /dev/pcm (pcm_* 0x297c..0x2d48, mvTdm.c 0x11dc..0x21a8)

[V] unless marked:
- `mvTdmInit(conn_mode, devs=2)`: linear mode (2 octets per sample), 80
  samples per period, "2 interrupts per period"; per channel 3 rx and 3 tx
  buffers of 160 octets (80 samples), DMA-coherent; staging buffers of
  2*160 for read and write.
- read: length min(count, 320); copies the buffer at each enabled channel's
  read index to `staging + ch*160` (a disabled channel's half keeps old
  data), advances the indices **whether or not the hardware has filled
  them**, decrements the ready counter, copies 320 octets out and returns
  **count**. No rx channel enabled: O_NONBLOCK -> -1 (EPERM), else sleeps.
- write: the same the other way; a full ring is overwritten.
- poll: IN|RDNORM if an rx channel is on and `rx_ready && tx_ready`;
  OUT|WRNORM if a tx channel is on and the same condition. So both come
  together, once per period.
- per period and channel, from the audio interrupt (`mvTdmChRxLow`,
  `mvTdmChTxLow`): the next of the 3 buffers is handed to the hardware side;
  if it is the one user space is at, that is an overflow/underflow: the
  same buffer is used again (rx: the new period is lost; tx: the last
  period is sent again) and a message is printed every 8th time. The ready
  counters go up (max 2) once all enabled channels have finished the period,
  but only from the third period after start.
- "an overrun stops the channel" (obiline's comment): the module's
  interrupt handler disables a channel on cause bits 0x02 (tx underflow) and
  0x20 (rx overflow), which come from `mv_ffxav_tdm_stop` and from the audio
  block's error paths, not from a slow reader [V 0x1b44, I for the cause].

Ours (TDM-free semantics the same for obiline): read blocks (or EAGAIN)
until a block is there and returns 320; write never blocks, and a full ring
drops the oldest queued block (counted); poll gives IN and OUT together once
per block, when a block has come in and the write credit (one per block,
max 2, as the module's tx_ready) is not used up. Receive overrun: the new
block is dropped (as the module) and counted; transmit underrun: silence is
sent (the module repeats the old block) and counted. Neither stops anything.

In a /dev/pcm block, sample i of channel c is the little-endian 16-bit
word at octet 2*(80*c + i): built from frame i of the descriptor as
`frame[off+1] | frame[off] << 8` (off = 0 or 10), as the module does.

## Debugging aids

- probe prints the audio block's registers (`dev_info`), and the SLIC's
  bus parameters when the SPI child binds.
- `/sys/devices/platform/soc/80018000.tdm/stats` (path per the tree):
  irqs, frames, blocks, reads, writes, rx_overruns, tx_underruns,
  tx_overwrites, hw_errors, queue fill, channel state, SLIC access counts.
- `.../regs`: the audio block's registers now.
- `ffxav_tel.spi_framing=` (module parameter, also writable in
  /sys/module/ffxav_tel/parameters) for the SLIC access framing.

## Check first on hardware (in this order)

1. Boot log: `as found:`, `rings 0003 running: taking them over`, the
   `tx ring:`/`rx ring:` lines, `adopted:` and `TDM: adopted, buffers at
   ...`. No "DMA unpaced" line. `cat .../80018000.tdm/stats` twice a
   second apart: `irqs` +~200, `frames` +8000, `blocks` +100. If `irqs`
   stays 0: TDM_CTRL bit 0 (never written by the stock) or the clock route.
2. SLIC: `obiline -v` (or a 10-line test) after `ioctl(0x71b6, 1)`: register
   0 through 0xc00171b2 on /dev/fxs0 reads **0xC3**. If not, try
   `echo 1 > /sys/module/ffxav_tel/parameters/spi_framing` (then 2), and
   check the SPI controller's chip select (SER bit 1).
3. /dev/pcm: 100 reads and 100 writes a second, `rx_overruns` and
   `tx_underruns` flat while obiline runs.
4. Audio: SLIC digital loopback or a tone. If the samples look byte-swapped
   or shifted: `lsb_first=1`, or another `chan_off`. If the far end hears
   nothing: `tx_lead=2`. Compare the descriptor contents (`regs`, devmem at
   the `rings at` addresses) with what is sent.
5. A register snapshot of a running stock unit (devmem 0x80018000..0x80018210,
   0x8001b400..0x8001b8b0, 0x8000f0a8..0x8000f0dc) settles most [G]/[I]
   items of the audio block, TDM_CTRL bit 0 first.
