# Si3226x / Si3217x ProSLIC facts for a from-scratch HT802 driver

Collected 2026-11-14. Read-only research: nothing on the device was touched.
Everything below is described in my own words; no vendor code and no patch
words are reproduced. Numbers are register addresses, bit positions and
configuration constants (facts), each with its source.

**Read section 0 first. The stock material says this particular board
(HT802 V2.5A) is probably NOT a Si32260.**

## Source keys

| Key | Source |
|---|---|
| API702 | Silicon Labs ProSLIC API 7.0.2 as shipped inside a Realtek vendor SDK mirrored on GitHub: `https://github.com/cubieb/2PORTONT/tree/master/sdk/rtk_voip/kernel/voip_drivers/proslic_api-v7.0.2` (files `inc/si3226x_registers.h`, `inc/si3226x_intf.h`, `inc/si3226x.h`, `inc/proslic.h`, `inc/si_voice.h`, `src/si3226x_intf.c`, `src/si_voice.c`, `src/rtk_proslic_wrapper/proslic_ctrl_spi.c`, `config_src/si3226x_LCQCUK_constants.c`, `config_inc/proslic_api_config.h`, `patch_files/si3226x_patch_C_FB_2012DEC06.c`) |
| API732 | Same repository, `proslic_api-v7.3.2` (adds `si3217x_LCQCUK_constants.c`, `si3217x_patch_C_FB_2014JUN18.c`, `si3226x_patch_C_FB_2014JUN18.c`; `inc/si3217x*.h`, `src/si3217x_intf.c`) |
| DS | Skyworks Si32260/61 data sheet rev 1.4 (Oct 2021): `https://www.skyworksinc.com/-/media/SkyWorks/SL/documents/public/data-sheets/Si32260-61.pdf` |
| KO | Stock module `v90d/recon/fw/p65/prog/lib/drv_silabs.ko` (firmware 1.0.65.x, not stripped; symbol names and section offsets given) |
| BSP | Stock module `v90d/recon/fw/p65/prog/lib/bsp_ht.ko` |
| ATA | `v90d/recon/fw/p65/prog/bin/gs_ata` (strings only) |
| DIS | `v90d/recon/out/silabs_ctrl.dis` (disassembly of the control wrappers from the 1.0.10.6 module) |
| DTS | `v90d/recon/backup/board.dts` (this unit's live device tree) |
| GPL | Grandstream GPL kernel `.../scratchpad/x/k/linux-3.4.20` (`sound/soc/codecs/si32178.c`, `drivers/gs/gs-slic.c`, `drivers/spi/spi-dspg-dcx.c`, `arch/arm/mach-dvf/factory*.c`) |
| LOG | Third-party boot log of an older HT802 (firmware 1.0.0.24, V1.x hardware): `v90d/refs/ht802/tools/FirmwareDumps/HT802-1.0.0.24/bootlog.md` |

Other public mirrors of the same API that I located but did not need:
`wangyx0055/mt7688-linux` (`drivers/char/pcm/proslic_api`), `olforu/study`,
`Bonstra/barebox-2011.06.0-qnap_ts_x31-gpl` (`diags/proslic/proslic_api`),
`frederic/rtl819x-toolchain` (API 5.6.4L). The gfiber.googlesource.com mirror
returned "not found" today. DAHDI was not needed. No public forum statement
about patch-free operation was found (one web search, no hits).

Note on the status of these sources: the API files carry a Silicon Labs
"proprietary, no dissemination" header even though they are distributed in
vendor GPL kernel tarballs; the stock `drv_silabs.ko` (which is the same API
compiled in) declares `license=GPL` in its modinfo (KO `.modinfo`). I used
them only to learn numbers and sequences.

---

## 0. Which chip is on this board? (premise check)

The task assumes a Si32260 ("Chip Type 28", rev C). The local evidence says
otherwise for hardware V2.5A:

| Evidence | Where | What it says |
|---|---|---|
| Board table inside the stock BSP | BSP `.data` +0x78..+0x318, 48-byte rows `{model, hw-rev string, soc, num_dev, ch_per_dev, 0, 0, REN, 0, type, type, n}` decoded by me | HT802 default row: 1 device x 2 channels, type 0. HT802 hw "26": 1 x 2, type 2. **HT802 hw "25": 2 devices x 1 channel, type 1.** HT801 rows: 1 x 1, type 1 |
| Type names in the stock driver | KO strings, in order: `SI3226X`, `SI3217X`, `SI3228X`; `VINETIC_InitChMember` (KO .text 0x3e00) switches on two type fields with values 0..3 and logs "Type = %d VirtualType = %d" | type 0 = Si3226x, 1 = Si3217x, 2 = Si3228x (order inferred from string order and from the HT801 being a single-channel product) |
| This unit's hw_features | HARDWARE.md | `si3217x_dev=1`, `num_slic_dev=2`, `num_ch_per_slic=1`: matches the "25" row exactly |
| This unit's device tree | DTS lines 213-237, 332-336 | two SPI slave nodes (`reg` 0 and 1), each with its own `rst-gpio`, two chip selects, two interrupt GPIOs |
| "Chip Type 28 / Rev 3 / LCQCUK / 1 SLIC device with 2 channel" | LOG lines 280-313 | This comes from a V1.x unit running 1.0.0.24. I found no dmesg capture from this V2.5A unit anywhere under `~/v90d` |

Conclusion (inference, strong): **this unit most likely carries two
single-channel Si3217x ProSLICs (part numbers Si32171..Si32179), one per
FXS port, each on its own chip select, reset and interrupt line.** FXS
port 1 would then be SPI device 0, ProSLIC channel 0 (CID 0).

Why it matters little for the register work and a lot for details:

* The direct register map 0..99 and 126 is the same in both families for
  everything in this document (API732 `si3217x_common_registers.h` versus
  API702 `si3226x_registers.h`, compared name by name for regs 0..30, 34,
  38, 47, 80, 81, 126). RAM addresses used below are also the same for the
  ones I spot-checked (MADC_VBAT 3, V_VLIM 640, VBATH_EXPECT 767, RINGFR
  844, LCRONHK 853, PRAM_ADDR 1358, PRAM_DATA 1359, PD_DCDC 1538,
  DCDC_OITHRESH 1548, DCDC_STATUS 1551).
* Differences: ID decoding, revision code for "rev C" (Si3217x: 2;
  Si3226x: 3), the patch (different blob), the general-parameter set,
  the DC-DC power-up sequence details, and which RAM locations are valid.
  See 4.6.

How to settle it without risk (all under the running stock firmware):

1. `dmesg | grep -i 'chip\|patch\|DCDC'` on the unit. The stock driver
   prints "Channel %d : Chip Type %d" and "loading patch: %08lx". Type
   20..27 = Si3217x, 28..37 = Si3226x, 50..59 = Si3228x (dispatch ranges in
   KO `ProSLIC_GetPCMPreset` 0x5f1c; enum order from API702 `si_voice.h`).
   Patch ID 0x06182014 = Si3217x rev C, 0x12062012 = Si3226x rev C.
2. The stock driver exposes `/proc/driver/si32260-cpe/` with entries
   `version`, `configure`, `rd<N>` (register dump), `ramd<N>` (RAM dump),
   `reg<N>`, `ram<N>`, `pc<N>` (power converter status) (KO strings at
   .rodata.str "driver/si32260-cpe", "rd%d", "ramd%d", "reg%d", "pc%d",
   "ram%d"; handlers `VINETIC_DumpReg_Proc` 0x11f4, `VINETIC_DumpRam_Proc`
   0x117c). **A register dump and a RAM dump taken from the working stock
   system is the ground truth for every value in this document**, including
   the DC-DC parameters, PCM mode and time slots. Take it before unloading
   the stock driver.

The rest of the document is written for Si3226x as requested, with the
Si3217x differences called out.

---

## 1. SPI protocol

### 1.1 Electrical

| Item | Value | Source |
|---|---|---|
| Mode | CPOL=1, CPHA=1 (Linux `SPI_MODE_3`), 8 bits per word, MSB first | GPL `sound/soc/codecs/si32178.c:909-910`, `drivers/gs/gs-slic.c:112-113`. DS Table 8 agrees: SDI is set up to and held from SCLK rising edge, SDO changes on SCLK falling edge |
| SCLK | min cycle 62 ns (about 16 MHz max); the board runs 1 MHz | DS Table 8; DTS `spi-max-frequency = <0xf4240>` |
| CS framing | CS must go high between bytes: every byte is its own CS-low frame. Min CS high time 220 ns | DS Table 8 "Delay time between chip selects"; KO `ctrl_ReadRegisterWrapper` 0x28e70 and `ctrl_WriteRegisterWrapper` 0x28f1c issue three separate one-byte `spi_write_then_read` calls per access, so CS toggles per byte; same in DIS 0x1bdc8 / 0x1be74 |
| Chip select | GPIO driven by the SPI controller driver | GPL `spi-dspg-dcx.c:111-117, 397-399`; DTS `current_cs_gpio = <&gpio 3 0x10 0  &gpio 3 0x11 0>` |
| 16-bit controllers | The part also accepts 16-bit frames | DS section 6.1 (not used here) |

### 1.2 Control byte

| Bit | Name | Meaning | Source |
|---|---|---|---|
| 7 | BRDCST | 1 = all channels on the chain accept the write | API702 `proslic_ctrl_spi.c` (broadcast write uses 0x80 or-ed with 0x20) |
| 6 | R/W | 1 = read, 0 = write | KO read wrapper ors 0x60, write wrapper ors 0x20 |
| 5 | REG | 1 = direct register access. Both public and stock code always set it; RAM is reached indirectly through registers 4..10 (section 3) | same |
| 4:0 | CID | channel number with its bits reversed: channel bit0 goes to control bit4, bit1 to bit3, bit2 stays, bit3 to bit1, bit4 to bit0 | KO `channel2cid` 0x28e3c; API702 `CNUM_TO_CID_QUAD` macro |

So channel 0: read 0x60, write 0x20. Channel 1: read 0x70, write 0x30.
Broadcast write: 0xA0.

### 1.3 Byte sequences

* Register write: frame 1 = control byte, frame 2 = register address,
  frame 3 = data. Three CS pulses.
* Register read: frame 1 = control byte (with bit 6), frame 2 = register
  address, frame 3 = clock out 8 bits and take SDO (what is sent on SDI in
  frame 3 does not matter).

Source: KO 0x28e70 / 0x28f1c, DIS 0x1bdc8 / 0x1be74.

### 1.4 Two channels in one package, daisy chain

* A dual Si3226x has one CS; its two channels are CID 0 and 1. Each FXS
  port is one SPI channel (DS 6.1).
* SDITHRU (8x8 mm package only) lets up to 16 channels share one CS; the
  CID then selects the channel along the chain (DS 6.1, pin table). RST
  must be held for at least 33 PCLK periods in this mode (DS Table 7; the
  Si3217x data sheet words it as "RSTb pulse width, SPI daisy chain mode").
* On this V2.5A board there are two chip selects and (probably) two
  single-channel parts, so each device is addressed as channel 0 on its
  own CS. Caveat: the 2017 GPL `spi-dspg-dcx.c` only reads the first
  `current_cs_gpio` entry; the shipped kernel is newer and may differ.
  How the stock driver maps "channel" to SPI device on a 2 x 1 board was
  not traced (per-device `spi` pointer comes from BSP `ht_get_host_access`).

---

## 2. Direct registers needed for bring-up

Addresses from API702 `si3226x_registers.h` (enum REGISTERS). Bit meanings
come from how API702 `si3226x_intf.c` uses them; where I infer, I say so.

| # | Name | Bits / use | Typical |
|---|---|---|---|
| 0 | ID | [2:0] revision, [5:3] part number low bits. Si3226x: rev C reads 3 (the header notes rev C reports the "rev D" code). Si3217x: rev A/B/C = 0/1/2, part = SI32171 + ID[5:3] | read only |
| 1 | RESET | soft reset: write 1 for channel 0, 2 for channel 1 (used by re-init) | 0 |
| 2 | MSTREN | master init enable (name only; API does not write it) | |
| 3 | MSTRSTAT | master status, PLL lock (name only; **the public init never reads it**, it relies on SPI readback working instead) | |
| 4 | RAMSTAT | bit 0 = RAM access busy | poll until 0 |
| 5 | RAM_ADDR_HI | bits [7:5] = RAM address bits [10:8] | |
| 6 | RAM_DATA_B0 | bits [7:3] = RAM data bits [4:0] | |
| 7 | RAM_DATA_B1 | RAM data bits [12:5] | |
| 8 | RAM_DATA_B2 | RAM data bits [20:13] | |
| 9 | RAM_DATA_B3 | RAM data bits [28:21] | |
| 10 | RAM_ADDR_LO | RAM address bits [7:0]; writing it starts the access | |
| 11 | PCMMODE | [1:0] format: 0 A-law, 1 mu-law, 3 16-bit linear; [2] A-law inversion option (preset field shifted by 2); [4] PCM enable; [5] PCM_TRI (which PCLK edge DTX tri-states on) | 0x13 = linear, enabled |
| 12 | PCMTXLO | transmit start count, low 8 bits | slot x width |
| 13 | PCMTXHI | [1:0] transmit start count high bits; [4] TX_EDGE (0 = DTX driven on PCLK rising edge). Also the presence probe: write 0x13, read back 0x13 = ProSLIC, 0x4x = DAA, 0x00/0xFF = nothing | |
| 14 | PCMRXLO | receive start count low. Also SPI sanity: write 0x5A, read back | |
| 15 | PCMRXHI | [1:0] receive start count high bits | |
| 16 | IRQ | interrupt summary (name only) | |
| 17 | IRQ0 | per-channel summary of which IRQn is pending (name only, inferred) | |
| 18 | IRQ1 | b0 OSC1_T1, b1 OSC1_T2, b2 OSC2_T1, b3 OSC2_T2, b4 RING_T1, b5 RING_T2, b6 FSKBUF_AVAIL, b7 VBAT | read clears (SPI mode) |
| 19 | IRQ2 | b0 RING_TRIP, b1 LOOP_STAT (hook change), b2 LONG_STAT, b3 VOC_TRACK, b4 DTMF, b5 INDIRECT, b6 TXMDM, b7 RXMDM | read clears |
| 20 | IRQ3 | b0 P_HVIC, b1 P_THERM, b2 PQ3, b3 PQ4, b4 PQ5, b5 PQ6, b6 DSP, b7 MADC_FS (power and thermal alarms) | read clears |
| 21 | IRQ4 | b0..b7 USER_0..7 | read clears |
| 22..25 | IRQEN1..4 | enable masks, same bit layout as IRQ1..4 | stock: 0x00, 0x13, 0x07, 0x00 (see 6.4) |
| 26 | CALR0 | calibration select; low nibble 0x0F = all longitudinal balance cals | |
| 27 | CALR1 | calibration select | 0xC0 for the standard set on rev C |
| 28 | CALR2 | calibration select; bit 0 = MADC offset | 0x01 first pass, 0x18 second pass |
| 29 | CALR3 | bit 7 = CAL_EN: write 1 to start, reads 1 while running | 0x80 |
| 30 | LINEFEED | [3:0] requested state: 0 open, 1 forward active, 2 forward on-hook transmission, 3 tip open, 4 ringing, 5 reverse active, 6 reverse OHT, 7 ring open. [6:4] = state actually in effect (inferred from the masks 0x44 and 0x11 used in the API) | 1 |
| 31 | POLREV | b0 abrupt, b1 start reversal, b2 wink (6 = wink start, 4 = wink stop) | 0 |
| 34 | LCRRTP | b1 = loop closure detected (off-hook); b0 = ring trip (inferred from the name and from ring-trip handling; only b1 is tested in public code) | |
| 35 | OFFLOAD | power offload (fixed-rail designs only) | |
| 38 | RINGCON | ringing control: trapezoid/sine select, cadence timer enables, unbalanced ringing (value comes from the ring preset) | preset |
| 39..42 | RINGTALO/HI, RINGTILO/HI | ring cadence active and inactive timers | preset |
| 43 | LOOPBACK | loopback modes | 0 |
| 44 | DIGCON | digital path control; bits 2 and 3 disable the TX/RX high-pass filters; also mute bits | |
| 45 | RA | impedance synthesis coarse value (from impedance preset) | preset |
| 46 | ZCAL_EN | automatic impedance calibration enable | 0x04 |
| 47 | ENHANCE | b0 wideband; b2 PLL free-run; b4 power-save enable (API masks with 0x07 to turn power-save off during bring-up); b7 = part number MSB on Si3226x | stock: 0x00 (Si3226x), 0x20 (Si3217x) |
| 60 | TONDTMF | [3:0] detected DTMF digit | |
| 66 | USERSTAT | from ring preset | |
| 67..70 | GPIO, GPIO_CFG1..3 | init sets GPIO_CFG1 = (old and 0xF9) or 0x60: GPIO1/2 as analog coarse sensor inputs | |
| 80 | AUTO | automatic state-machine controls; 0x2F during init, then the configured value | 0x3F |
| 81 | JMPEN | 1 = patch jump table enabled | 1 after patch |
| 82..97 | JMP0LO/HI .. JMP7LO/HI | low 8 patch jump entries, 16 bits each, low byte first | |
| 98 | PDN | 0x80 written at init ("MADC powered in open state") | 0x80 |
| 126 | USERMODE_ENABLE | b0 reads 1 while user mode is on. Toggle sequence: write 0x02, 0x08, 0x0E, 0x00 in that order. The same sequence switches it back off | |

Audio gain, hybrid and impedance coefficients are RAM, not registers; see 3.3.

Calibrations run at init by the public driver (API702 `Si3226x_Init_with_Options`,
`Si3226x_Calibrate`, `Si3226x_LBCal`, `proslic.h` timeouts):

1. Before the DC-DC is started: CALR0..3 = 0x00, 0x00, 0x01, 0x80 (MADC
   offset only), poll CALR3 bit 7 every 10 ms, give up after 100 polls.
2. After VBAT is up: CALR0..3 = 0x00, 0xC0, 0x18, 0x80, up to 300 polls.
3. Longitudinal balance calibration (separate call, needs the line on-hook
   and takes seconds): power-save off, LINEFEED = forward active,
   CALR0 = 0x0F, CALR3 = 0x80, poll up to 2410 x 10 ms, restore LINEFEED
   and ENHANCE. Results can be read and reloaded later (RAM CMDAC_FWD
   1476, CMDAC_REV 1477, CAL_TRNRD_DACT 1458, CAL_TRNRD_DACR 1459); the
   stock driver saves and reloads them (LOG "LOAD: cal_results1..4").
4. Impedance preset load ends by waiting for CALR3 bit 7 to clear
   (ZCAL, up to 500 ms).

---

## 3. RAM (indirect) access

### 3.1 Procedure

Identical in the public wrapper (API702 `proslic_ctrl_spi.c`) and the
stock module (KO `ctrl_WriteRAMWrapper` 0x29020, `ctrl_ReadRAMWrapper`
0x290f4, `__wait_access` 0x28fcc).

Write RAM[addr] = value (29-bit value):

1. Poll reg 4 until bit 0 is 0 (stock: up to 60 back-to-back reads, then
   it logs an error and carries on).
2. reg 5 = (addr >> 3) and 0xE0.
3. reg 6 = (value << 3) and 0xF8; reg 7 = value >> 5; reg 8 = value >> 13;
   reg 9 = value >> 21 (each truncated to 8 bits).
4. reg 10 = addr and 0xFF. This commits the write.
5. Poll reg 4 again until not busy.

Read RAM[addr]:

1. Poll reg 4. 2. reg 5 = (addr >> 3) and 0xE0. 3. reg 10 = addr and 0xFF.
4. Poll reg 4. 5. Read regs 6, 7, 8, 9;
   value = (reg6 >> 3) + (reg7 << 5) + (reg8 << 13) + (reg9 << 21).

Notes:

* Values are 29 bits; bit 28 is the sign for signed quantities (the API
  sign-extends MADC readings when bit 28 is set).
* Addresses are 11 bits (0..2047). Addresses 0..1023 are ordinary RAM.
  1024 and up are "memory-mapped registers" and **need user mode**
  (section 2, reg 126). Without user mode those writes are ignored.
* The 1.0.10.6 module masked reg 6 with 0xF0 instead of 0xF8 (DIS 0x1bfcc);
  1.0.65 uses 0xF8. Use 0xF8.
* Broadcast RAM writes use the broadcast control byte for every register
  write of the sequence.
* Self-test used by the API: write 0x12345678 to RAM 449 (an unused
  location) and read it back (29 bits survive).

### 3.2 Scaling facts

| Quantity | Scale | Source |
|---|---|---|
| Voltages from the monitor ADC (MADC_VBAT etc.) and the *_EXPECT targets | 1 mV = 1074 counts (LSB 931.32 nV) | API702 `proslic.h` SCALE_V_MADC; check: VBATH_EXPECT 0x03D70A20 / 1074 = 59.985 V, exactly the "Vbath" in LOG line 308 |
| "5 V" comparison constant | 0x51EB82 | `proslic.h` COMP_5V |

### 3.3 RAM locations that matter (Si3226x rev C numbering)

DC-DC control and status (all at 1024+, user mode required):

| Addr | Name | Use |
|---|---|---|
| 1538 | PD_DCDC | converter power state. Bit 20 (0x100000) set = powered down. Values written by the API: 0x700000 clear OV/UV lockout, 0x600000 start, 0x400000 run (analog loop), 0x300000 emergency shutdown, 0x900000 orderly power-down |
| 1540..1542 | PD_UVLO, PD_OVLO, PD_OCLO | under/over-voltage and over-current lockout enables |
| 1545..1548 | DCDC_UVHYST, DCDC_UVTHRESH, DCDC_OVTHRESH, DCDC_OITHRESH | protection thresholds |
| 1551 | DCDC_STATUS | fault/status; written 0 to clear after start |
| 1552 | DCDC_FSW | switching frequency |
| 1553 | DCDC_SWDRV_POL | **switch drive polarity** (0 or 0x100000) |
| 1554 | DCDC_UVPOL | UV comparator polarity |
| 1555 | DCDC_CPUMP | charge pump: 0x100000 on, 0 off |
| 1558 | DCDC_VREF_CTRL | |
| 1560 | DCDC_RNGTYPE | ring type seen by the converter (also written by the ring preset) |
| 1565 | DCDC_OIMASK | 0xC00000 for tracking supplies |
| 1585..1590 | DCDC_ANA_GAIN, _TOFF, _TONMIN, _TONMAX, _DSHIFT, _LPOLY | analog control loop: gain, off time, min/max on time |
| 1592 | DCDC_PD_ANA | |
| 1616 | DCDC_CPUMP_LP_MASK | 0x1100000 |
| 1641, 1643 | DCDC_UV_DEBOUNCE, DCDC_OV_DEBOUNCE | 0x200000 and 0 for tracking supplies |
| 1004..1007 | DCDC_VERR, DCDC_VERR_HYST, DCDC_OITHRESH_LO, DCDC_OITHRESH_HI | |
| 963 | DCDC_ANA_SCALE | |
| 919, 920 | VBAT_TRACK_MIN, VBAT_TRACK_MIN_RNG | minimum tracking battery, talk and ringing |
| 973..975 | VOV_DCDC_SLOPE, VOV_DCDC_OS, VOV_RING_BAT_MAX | ringing overhead |

Battery targets and monitor:

| Addr | Name | Use |
|---|---|---|
| 3 | MADC_VBAT | measured battery voltage |
| 6 | MADC_VDC | measured DC input |
| 1, 2, 12, 13 | MADC_VTIPC, MADC_VRINGC, VTIP, VRING | line voltages |
| 7..10 | MADC_ILONG, MADC_ITIP, MADC_IRING, MADC_ILOOP | line currents |
| 766, 767, 768 | VBATL_EXPECT, VBATH_EXPECT, VBATR_EXPECT | expected low / high (talk) / ringing battery |
| 764 | BAT_HYST | |
| 1020 | VBATH_DELTA | (rev C address; rev B used different slots) |
| 1021..1023 | UVTHRESH_MAX, UVTHRESH_SCALE, UVTHRESH_BIAS | |
| 756, 759 | P_TH_HVIC, COEF_P_HVIC | line driver power alarm threshold and filter |
| 402 | P_HVIC | computed line driver power |
| 915, 916 | PWRSAVE_TIMER, OFFHOOK_THRESH | |

DC feed, loop closure (the "DC feed preset", 24 values):

| Addr | Name |
|---|---|
| 634..639 | SLOPE_VLIM, SLOPE_RFEED, SLOPE_ILIM, SLOPE_RING, SLOPE_DELTA1, SLOPE_DELTA2 |
| 640..645 | V_VLIM (open-circuit voltage), V_RFEED, V_ILIM, CONST_RFEED, CONST_ILIM (loop current limit), I_VLIM |
| 852, 853 | LCROFFHK, LCRONHK (loop closure thresholds going off-hook / back on-hook) |
| 701, 702 | LCRDBI, LONGDBI (debounce) |
| 854..857 | LCRMASK, LCRMASK_POLREV, LCRMASK_STATE, LCRMASK_LINECAP |
| 858, 859 | LONGHITH, LONGLOTH |
| 748, 751, 752 | VCM_OH, VOV_GND, VOV_BAT |

The public example preset is "48 V open circuit, 20 mA limit, loop closure
at 10/12 mA, 5 ms debounce" (API702 `si3226x_LCQCUK_constants.c`).
The DC feed load brackets the writes with LINEFEED = 0 and restores it.

Ring generator (the "ring preset"):

| Addr | Name |
|---|---|
| 844, 845, 846, 843 | RINGFR (frequency coefficient), RINGAMP, RINGPHAS, RINGOF (DC offset) |
| 755 | RTPER |
| 637 | SLOPE_RING |
| 860 | IRING_LIM |
| 847..850 | RTDCTH, RTACTH (ring trip thresholds), RTDCDB, RTACDB (debounce) |
| 753, 896 | VOV_RING_BAT, VOV_RING_GND |
| 749, 482, 483 | VCM_RING, VCM_RING_FIXED, DELTA_VCM |
| 768 | VBATR_EXPECT (clamped to the general-configuration value) |

plus registers 38..42 and 66, and DCDC_RNGTYPE (user mode).

Audio path (the "impedance preset"), all plain RAM:

| Addr | Name |
|---|---|
| 540..543 | TXACEQ_C0..C3 |
| 546..549 | RXACEQ_C0..C3 |
| 563..570 | ECFIR_C2..C9 (hybrid FIR) |
| 571..574 | ECIIR_B0, B1, A1, A2 (hybrid IIR) |
| 653..657 | ZSYNTH_B0, B1, B2, A1, A2 (impedance synthesis), plus reg 45 RA |
| 544, 545, 906 | TXACGAIN, RXACGAIN, RXACGAIN_SAVE |
| 658..660 | RXACHPF_B0_1, B1_1, A1_1 |
| 518..530, 550..562 | TXACIIR_*, RXACIIR_* (band filters, loaded by the PCM setup) |
| 531..539, 658..666 | TXACHPF_*, RXACHPF_* (wideband only) |

Patch loading:

| Addr | Name | Use |
|---|---|---|
| 1358 | PRAM_ADDR | patch RAM address pointer (= 334 + 1024); address is written shifted left by 19 |
| 1359 | PRAM_DATA | patch RAM data (= 335 + 1024); each word is written shifted left by 9; the pointer auto-increments |
| 1597..1604 | PATCH_JMP8..15 | upper 8 jump entries, 13 bits each |
| 448 | PATCH_ID | patch serial, written last; readable to see which patch is loaded |
| 1499 | RDC_SUM | bit 22 checked together with LINEFEED to detect a corrupted state after an improper ring exit |

---

## 4. Standard init sequence (public driver, API702 `Si3226x_Init_with_Options`)

Steps marked **[P]** depend on the patch.

1. **Clocks first.** PCLK and FSYNC must be present and constant (DS
   Table 9 note 3). The PLL locks to PCLK; nothing else works without it.
2. **Reset.** Public generic helper: assert RST, wait 250 ms, release,
   wait 250 ms (`si_voice.c` `SiVoice_Reset`). Data sheet minimum: RST low
   for 33 PCLK periods (DS Table 7). Grandstream's GPL helper: high
   100 us, low 1 ms, high 1 ms (GPL `si32178.c:816-823`). The stock BSP
   does assert, delay loop, release, delay loop (BSP `ht_bsp_ioctl`, 101
   iterations of a busy-wait each; the per-iteration time was not decoded).
3. **Wait for PLL lock / SPI alive.** The public code does not read
   MSTRSTAT. It probes: write 0x13 to reg 13 and read it back twice
   (expect 0x13), then write 0x5A to reg 14 and read it back. Failure
   stops the init.
4. **Identify.** Read reg 0 (rev = bits 2:0) and reg 47 (bit 7).
   Si3226x part number = (reg47 bit 7 as bit 3) + ID[5:3]: 0 = Si32260,
   4 = Si32360, 8 = Si32266, 10 = Si32268.
   **Only rev C (code 3) is accepted; any other revision aborts with
   "unsupported device revision".**
5. **[P] Load patch**, per channel (or broadcast):
   user mode on; reg 81 = 0; zero the 16 jump entries (regs 82..97 and RAM
   1597..1604); RAM 1358 = 0; write each patch word (shifted left 9) to
   RAM 1359 until the terminating zero word (for broadcast on old
   revisions the pointer is rewritten before every word); wait 1 ms;
   reg 5 = 0; write the non-zero jump entries; RAM 448 = patch ID; write
   the patch "support RAM" list (address/value pairs); user mode off.
6. **[P] Verify patch** (read PRAM back through RAM 1359 after resetting
   the pointer, read back jump entries and support RAM), then **enable**:
   user mode on, reg 81 = 1, user mode off.
7. **General parameters** (user mode on, per channel): reg 47 = 0,
   reg 80 = 0x2F, then about 40 RAM writes from the general configuration
   (BAT_HYST, VBATR/VBATH_EXPECT, PWRSAVE_TIMER, OFFHOOK_THRESH,
   VBAT_TRACK_MIN(_RNG), THERM_DBI, DCDC_VERR(_HYST), DCDC_OITHRESH_LO/HI,
   PD_UVLO/OVLO/OCLO, DCDC_UVHYST/UVTHRESH/OVTHRESH, DCDC_UVPOL,
   DCDC_VREF_CTRL, DCDC_RNGTYPE, DCDC_ANA_GAIN/TOFF/TONMIN/TONMAX/DSHIFT/
   LPOLY, COEF_P_HVIC, P_TH_HVIC, SCALE_KAUDIO, LKG_OFHK_OFFSET,
   LKG_LB_OFFSET, VBATH_DELTA, UVTHRESH_MAX/SCALE/BIAS), then fixed
   tweaks: GPIO_CFG1 analog mode, reg 98 = 0x80, TXACHPF_A1_1, ROW0_C2,
   ROW1_C2 (DTMF), XTALK_TIMER, DCDC_CPUMP_LP_MASK, the three VOV_DCDC
   values, VDIFFLPF, ILOOPLPF, ILONGLPF, VCMLPF, CM_SPEEDUP_TIMER, VCM_TH;
   for tracking supplies also DCDC_UV_DEBOUNCE, DCDC_OV_DEBOUNCE,
   DCDC_OIMASK, VCM_HYST, LPR_SCALE, LPR_CM_OS; finally DCDC_SWDRV_POL
   (inverted if the board flag "dcdc polarity invert" is set).
   A guard refuses to run if the configuration's device key is outside
   0x64..0x6D, "to prevent high current states" from a wrong table.
8. **MADC offset calibration** (section 2, pass 1).
9. **DC-DC power-up** (tracking supply variant, `Si3226x_PowerUpConverter`),
   user mode on:
   skip if RAM 1538 bit 20 is already clear; write DCDC_OITHRESH from the
   "lo" value; LINEFEED = open; save reg 47 and clear its power-save
   bits; PD_DCDC = 0x700000; charge pump on (DCDC_CPUMP = 0x100000);
   wait 10 ms; PD_DCDC = 0x600000; wait 50 ms; read VBATH_EXPECT and
   MADC_VBAT; **if VBAT is below half of the target, write
   PD_DCDC = 0x300000 and disable the channel (short-circuit rule)**;
   otherwise DCDC_STATUS = 0, PD_DCDC = 0x400000, restore reg 47, wait
   50 ms; then poll MADC_VBAT every 10 ms for up to 2 s until it is within
   about 5 V of VBATH_EXPECT; on timeout shut down with 0x300000.
   Fixed-rail (TSS) boards use a different branch (odd channel only, 500 ms
   wait). Power-down: LINEFEED = 2 then 0, wait 50 ms, PD_DCDC = 0x900000,
   wait until VBAT is under 5 V.
10. **Remaining calibrations** (section 2, pass 2).
11. Write the configured reg 47, reg 80 and reg 46 values.
12. Outside `Init` (done by the application; stock order is in 6.5):
    longitudinal balance calibration or reload of saved results, DC feed
    preset, ring preset, impedance preset, PCM setup and time slots, PCM
    enable, interrupt enables, and finally LINEFEED = 1 (forward active).
13. Runtime: on interrupt read regs 18..21 (reading clears them), hook
    state is reg 34 bit 1, ring by writing LINEFEED = 4 and stop by
    writing 1. The API clears IRQEN1 bit 7 (VBAT) while ringing.

### 4.1 What the patch is, as far as public material shows

* It is a block of DSP program words for the chip's patch RAM plus a jump
  table: up to 16 entry points of the ROM firmware are redirected
  (8 through regs 82..97, 8 through RAM 1597..1604), enabled by reg 81.
* The Si3226x rev C patch dated 2012-12-06 (ID 0x12062012) has 229
  program words, uses 8 jump entries and sets 16 support RAM locations:
  782, 786, 787, 794, 795, 798, 799, 800, 892, 893, 925, 926, 1014,
  1020, 1021, 1022 (API702 patch file; counted by me).
  925/926 are aliased in the register header as the "obsolete" DCDC_DIN_LIM
  and DCDC_VOUT_LIM; 1020..1022 are VBATH_DELTA and the UVTHRESH pair.
  So at least part of it concerns the DC-DC controller and battery
  under-voltage handling (inference from addresses, not a documented
  statement). The 2014-06-18 revision (ID 0x06182014) is larger: 253
  words, 19 support locations (adds 333, 334, 352).
* One patch serves every tracking topology: `proslic_api_config.h` maps
  flyback, Cuk, quasi-Cuk, low-cost quasi-Cuk (LCQCUK) and PMOS
  buck-boost to the same "RevC Flbk" patch. Only the fixed-rail (TSS)
  and isolated TSS designs have their own. So the patch is not what
  adapts the chip to LCQCUK; the general parameters do that.

### 4.2 Running rev C without the patch: what is known

* **No public source states that rev C works without it.** The public
  driver has no patch-free path for normal use: rev C always gets a
  patch, every other revision is rejected. There is an init option
  "no patch load" but it is meant for re-init when the patch is already
  in RAM.
* No "no patch required" revision exists in the Si3226x or Si3217x
  support code of API 7.0.2 / 7.3.2. (Si3217x rev B and rev C both have
  patches. The stock module also carries a patch for Si3228x rev A.)
* The patch survives soft re-init but not a hardware reset, so the stock
  system reloads it on every start (LOG lines 306-307, about 0.5 s per
  channel at 1 MHz byte-framed SPI).
* What is likely to work unpatched (inference): SPI, RAM access, PCM
  codec path, register state machine. These are ROM functions and the
  API exercises SPI/RAM before the patch is loaded.
* What is at risk unpatched (inference): DC-DC regulation and its
  protection (see the support RAM addresses), power-save transitions,
  ring entry/exit. An errata fix in exactly the block that generates
  -60..-110 V is the worst place to guess.
* Practical consequence: patch-free behaviour can only be established by
  experiment, and the experiment involves the DC-DC converter. If tried,
  do it with the short-circuit rule from step 9, a current-limited supply
  or at least a hand on the kill switch, LINEFEED open, and compare
  MADC_VBAT, DCDC_STATUS and P_HVIC against a dump taken under stock.

### 4.3 The patches in the stock module are the public ones

Compared word for word by script (no data copied here):

| Stock symbol (KO .rodata) | Size | Patch ID | Identical to |
|---|---|---|---|
| `si3226xPatchRevCFlbk` 0x23dc (data 0x23f0, entries 0x2788, support 0x27a8/0x27cc) | 230 words incl. terminator, 16 entries, 16 support pairs | 0x12062012 | API702 `si3226x_patch_C_FB_2012DEC06.c`: data, entries, support addresses and support values all equal |
| `si3217xPatchRevCFlbk` 0x2088 (data 0x209c) | 176 words incl. terminator, 15 support pairs | 0x06182014 | API732 `si3217x_patch_C_FB_2014JUN18.c`: all equal |
| `si3228xPatchRevALCQC` 0x2810 (data 0x2824) | 387 words | not compared | no public counterpart fetched |

So "about 900 bytes" in HARDWARE.md is the 920-byte Si3226x table; the
Si3217x one is 704 bytes. Whether the user's own firmware may ship either
blob is a licensing question, not a technical one: the identical blobs
sit in several vendor GPL tarballs.

### 4.4 DC-DC topology: what depends on it and what goes wrong

The board type selects one row of the "general configuration" table
(`ProSLIC_Init_MultiBOM(preset)`). Topology-dependent members (API702
`Si3226x_General_Cfg`): bomOpt, batType, vdc_range, dcdc_oithresh_lo/hi,
dcdc_uvhyst, dcdc_uvthresh, dcdc_ovthresh, dcdc_oithresh, dcdc_swdrv_pol,
dcdc_uvpol, dcdc_vref_ctrl, dcdc_rngtype, dcdc_ana_vref, dcdc_ana_gain,
dcdc_ana_toff, dcdc_ana_tonmin, dcdc_ana_tonmax, dcdc_ana_dshift,
dcdc_ana_lpoly, dcdc_ana_scale, dcdc_cpump_lp, dcdc_verr(_hyst),
pd_uvlo/ovlo/oclo, vbat_track_min(_rng), vbath_expect, vbatr_expect,
coef_p_hvic, p_th_hvic, plus the charge pump handling in the power-up
sequence.

Failure modes (the first is stated by the API authors, the rest are my
engineering reading of the parameter names):

* Wrong table for the device: the API's own comment says it "could lead to
  improper initialization and high current states"; that is why the
  device-key guard exists.
* **DCDC_SWDRV_POL wrong**: the switch transistor is driven with inverted
  sense, i.e. held on when it should be off. Inductor saturates, the FET
  and inductor overheat within seconds. The stock table has polarity 0
  for LCQCUK and 0x100000 for PMOS buck-boost, so the two boards Grandstream
  supports are opposite. This is the single most dangerous value.
* On-time limits and off time (TONMIN/TONMAX/TOFF) too long for the
  inductor: saturation and over-current. Too short: VBAT never reaches
  the target (the init then times out and shuts down, which is benign).
* Over-current / over-voltage thresholds and lockouts (OITHRESH*, OVTHRESH,
  PD_OCLO/OVLO/UVLO) wrong or disabled: no protection when the line is
  shorted or the converter runs away; output capacitor and line driver
  voltage ratings can be exceeded.
* Charge pump setting wrong for the gate-drive arrangement: weak or no
  gate drive, FET in linear region, heat.
* VBATR_EXPECT / VOV values too high: ringing battery above what the
  board's capacitors and the line driver tolerate.
* P_TH_HVIC too high: thermal alarm arrives late.

Good news for this board: **the stock LCQCUK tables are almost the public
ones** (6.3), so nothing has to be guessed. Still, confirm the live values
with a RAM dump from the running stock system (section 0, item 2), because
the mapping from `slic_dcdc_type=2` to a table row is made in `gs_ata`
(closed, not traced). LOG shows "Init_MultiBOM for DCDC: LCQCUK" for a
V1.x Si32260 unit; for V2.5A it is unverified.

### 4.5 Timing seen in the field (V1.x unit, LOG)

Patch load 0.5 s per channel; DC-DC start to "VBAT up" about 0.15 s per
channel; measured VBAT 57.6..57.9 V against a 59.985 V target; PCM start
about 0.7 s later; whole SLIC init about 4 s.

### 4.6 If the chip is a Si3217x (likely here): differences

Source: API732 `src/si3217x_intf.c`, `inc/si3217x.h`,
`inc/si3217x_common_registers.h`, `inc/si3217x_revc_registers.h`.

* ID: part = SI32171 + ID[5:3] (enum values 20..27); rev A/B/C = 0/1/2.
  Reg 47 bit 7 is not part of the ID.
* Same SPI protocol, same RAM access, same user-mode key (reg 126 is
  named TEST_CNTL there), same patch mechanism and addresses (PRAM 1358 /
  1359, jump table regs 82..97 and RAM 1597..1604).
* Rev C calibration constants are the same (CALR1 0xC0, CALR2 0x18).
* Rev C has its own helper stage in the driver (stock symbols
  `Si3217x_RevC_GenParamUpdate`, `Si3217x_RevC_SelectPatch`,
  `Si3217x_RevC_ConverterSetup`, KO 0xe794 / 0xf078 / 0xf0f8) and a
  different general-configuration structure (54 members, 0xC4 bytes:
  i_oithresh*, v_ovthresh, v_uvthresh, v_uvhyst, dcdc_fsw_*, dcdc_din_lim,
  dcdc_vout_lim, vov_dcdc_*, gate-driver option, optional DAA).
  The converter power-up is `Si3217x_PowerUpConverter` (not transcribed
  here; read it before writing that part).
* Rev C RAM map differs in places (rev-specific header); do not reuse
  Si3226x RAM numbers above 1000 blindly: e.g. 1020/1021 are AMP_MOD_G /
  AMP_MOD_OS on Si3217x rev C, and LKG_LB_OFFSET / LKG_OFHK_OFFSET are
  1017 / 1018 (swapped relative to Si3226x rev C).
* Si3217x parts can include a DAA on the same die (Si32178/9); the
  presence probe on reg 13 distinguishes.

---

## 5. PCM bus requirements

| Item | Fact | Source |
|---|---|---|
| PCLK rates | 512 k, 768 k, 1.024 M, 1.536 M, 1.544 M, 2.048 M, 4.096 M, 8.192 MHz (period 122..1953 ns), 40..60 % duty, jitter 8 ns rms | DS Table 9 |
| FSYNC | 8 kHz (125 us), constant; short or long pulse both accepted; width from one PCLK period up to 125 us minus one period; sampled on PCLK falling edge (setup 25 ns, hold 20 ns) | DS Table 9, pin description of FSYNC |
| Clocks must run | "A constant PCLK and FSYNC are required"; they also clock the PLL and DSP, so they must be up before reset is released | DS Table 9 note 3 |
| Data edges | DRX sampled on PCLK falling edge; DTX driven from PCLK rising edge by default; TX_EDGE (reg 13 bit 4) and PCM_TRI (reg 11 bit 5) select the alternative edges | DS Table 9 (setup/hold DRX to PCLK fall, delay PCLK rise to DTX); API702 `Si3226x_PCMSetup` |
| Slot numbering | Start positions are counted in PCLK periods from FSYNC, 10 bits each for TX (regs 12/13) and RX (regs 14/15), independent of each other. 8 to 128 eight-bit slots per frame depending on PCLK | DS 6.2; API702 `Si3226x_PCMTimeSlotSetup` |
| Sample width | A-law or mu-law: 8 PCLK periods. 16-bit linear: 16 periods (two 8-bit slots), MSB first. Wideband uses two slots per frame | DS 6.2 and 5.x; stock arithmetic in 6.2 |
| DTX | tri-stated outside the slot; INT pin can alternatively serve as DTX output enable | DS pin 12 description |
| Exact bit offset (does count 0 mean the bit at the FSYNC edge or one PCLK later) | not stated in the public data sheet text I could extract; determine with a scope or by loopback (reg 43) | open |

---

## 6. What the stock material reveals for this board

### 6.1 Hardware wiring

| Fact | Source |
|---|---|
| SPI controller `dspg,spi-dcx` at 0x05e00000, two slaves `dvf_si32178_spi@0` and `@1`, 1 MHz | DTS 213-237 |
| Chip selects: `current_cs_gpio = <phandle 3 0x10 0>, <phandle 3 0x11 0>` (three cells per GPIO: bank 3, pins 16 and 17) | DTS 220 |
| Resets: device 0 `rst-gpio = <phandle 4 0x15 0>` (bank 4 pin 21), device 1 `<phandle 4 0x13 0>` (bank 4 pin 19) | DTS 225, 233 |
| Interrupts: `int-gpio = <phandle 0 0x10 0>, <phandle 0 0x11 0>` (bank 0 pins 16 and 17), claimed as "Silabs_int", edge | DTS 335; BSP strings "gs-si3226x-int", "Silabs_int" |
| Note: HARDWARE.md lists these as flat numbers "CS 16/17, reset 21/19". The DTS has a bank cell; CS and INT have the same pin numbers in different banks. Compute the Linux GPIO numbers from the bank before poking them | DTS |
| SPI mode 3, 8-bit, one byte per message | GPL `si32178.c:909`, KO wrappers |
| TDM0 `dspg,tdm` at 0x05f00000 (reset line 9), ownership handed to the CSS DSP (`tdm-ownership = <3>`; LOG "Move TDM ownership to CSS") | DTS 282-305, LOG 430 |
| PCLK rate, FSYNC shape, who is clock master: not found in any local file. The SoC is the master by necessity (the SLIC only has clock inputs) | open |

### 6.2 PCM format and time slots

| Fact | Source |
|---|---|
| Slot per channel is stored in the channel object and logged "Silabs devices Channel = %d using PCM slot = %d"; on the V1.x unit channel 0 used slot 0 and channel 1 slot 1 | KO `VINETIC_InitChMember` 0x3e00 (store at +0x200); LOG 289, 292 |
| Start counts: TX count = RX count = slot x 8 for 8-bit formats, slot x 16 when the resolution code is above 1 | KO `IFX_TAPI_LL_PCM_Cfg` 0x48d4: width 8 or 16 chosen at 0x49b4, multiply at 0x4a40, then `ProSLIC_PCMTimeSlotSetup(ch, n, n)` |
| Default resolution code is 2 when the application passes no init block ("PCM Ch %d uses default PCM resolution setting"); highway 0; RX slot = TX slot | KO `VINETIC_PCM_baseConf` 0x4c60 |
| Stock PCM preset table (5 bytes each: format, wideband, tri, tx_edge, alaw_inv). Si3226x: [0] linear 16, [1] linear 16 wideband, [2] A-law, [3] mu-law, [4] linear 16. Si3217x: same but [2] mu-law, [3] A-law. All have tri = 0, tx_edge = 0, alaw_inv = 0 | KO .data `Si3226x_PCM_Presets` 0x1238, `Si3217x_PCM_Presets` 0x8c |
| After the PCM preset the stock driver disables the TX/RX high-pass filters (logs a warning if that fails), sets the PCM enable bit, then writes the slots | KO 0x49f0..0x4a48 call order |
| So the stock line format is most probably 16-bit linear, 8 kHz, DTX on rising edge, channel n at PCLK count 16 x n. The mapping from resolution code to preset index (`Si3226x_GetPCMPreset` 0x14d10) was not decoded; confirm by reading regs 11..15 from `/proc/driver/si32260-cpe/rd<N>` | inference + open |
| On a 2 x 1 board both devices share the PCM highway, so the second device needs a different slot than the first; value not verified for V2.5A | inference |

### 6.3 General configuration (DC-DC) tables in the stock module

Decoded with the public structure layouts and compared member by member.

Si3226x, `Si3226x_General_Configuration_MultiBOM` (KO .data 0x25d4, two
rows of 0xE0 bytes; names "LCQCUK" and "PMOS BB" from KO strings):

| Row | Differences from public API702 `si3226x_LCQCUK_constants.c` |
|---|---|
| 0, LCQCUK (bomOpt 3, tracking, VDC range code 1) | device_key 0x64 (public 0x65); vbatr_expect 0x070A3D3A = 110 V (public 0x051EB82A = 80 V); dcdc_ovthresh 0x01300000 (API702 has 0x00500000, API732 has 0x01300000); coef_p_hvic 0x07FE4B00 (public 0x07FEB800); p_th_hvic 0x0048D15B (public 0x005B05B2); irqen2 0x13 (public 0); irqen3 0x07 (public 0x03); enhance 0x00 (public 0x10). **All other 53 members are identical**, including vbath_expect 0x03D70A20 (60 V), swdrv_pol 0, ana_gain 0x00300000, ana_toff 0x00300000, ana_tonmin 0x00100000, ana_tonmax 0x00FFC000, ana_dshift 0x00F00000, ana_lpoly 0x0FDA4000, oithresh_hi 0x01C00000, AUTO 0x3F, ZCAL_EN 0x04 |
| 1, PMOS buck-boost (bomOpt 10) | as row 0 except dcdc_oithresh_hi 0x00D00000, dcdc_ovthresh 0x00D00000, **dcdc_swdrv_pol 0x00100000**, dcdc_ana_tonmax 0x01F00000 |

Si3217x, `Si3217x_General_Configuration_MultiBOM` (KO .data 0x10ac, one
row of 0xC4 bytes, bom option 3 = LCQCUK, no gate driver, VDC range code
1, no DAA):

| Differences from public API732 `si3217x_LCQCUK_constants.c` |
|---|
| device_key 0x77 (public 0x71); irqen2 0x13 (public 0); enhance 0x20 (public 0x30). **All 45 numeric RAM/threshold members are identical.** |

Runtime overrides announced by the stock init on the V1.x unit (LOG
296-299): "calibration disabled" (it reloads saved LB calibration
instead), "Overwriting IRING limit 45 mA", "Overwriting RTACTH limit to
32 mA", "VBAT tracking is disabled". The RAM values these produce were
not decoded; a RAM dump will show them.

### 6.4 Interrupt enables used by stock

IRQEN1 = 0x00, IRQEN2 = 0x13 (ring trip, loop status = hook change, DTMF),
IRQEN3 = 0x07 (line-driver power alarm, thermal alarm, PQ3), IRQEN4 = 0
(from 6.3 tables). Hook and DTMF therefore arrive by interrupt; VBAT
alarm is off.

### 6.5 Order of operations in the stock driver

| Step | Source |
|---|---|
| `insmod bsp_ht.ko major_num=122 bsp_device=slic_bsp`, then `drv_tapi.ko`, then `drv_silabs.ko`; device nodes `/dev/fxs00..03` major 121 | `v90d/recon/fw/p65/base/etc/rc.d/init.d/slic` |
| gs_ata opens `/dev/slic_bsp`, asks for board info, pulses the reset line through a BSP ioctl ("HW assert / de-assert Nuvoton chip Reset pin"), expects to detect "SLIC_SI32260" (it knows only that name and the Nuvoton N682386; other families are passed as a "virtual type") | ATA strings; BSP `ht_bsp_ioctl`; LOG 280-282 |
| Driver start registers the four control wrappers (register read/write, RAM read/write), delay and semaphore callbacks | KO `VINETIC_DeviceDriverStart` 0x459c |
| Per device: get SPI handle and IRQ from the BSP, connect then disable the IRQ | KO `VINETIC_InitDevMember` 0x3fd0; LOG 285-289 |
| Per channel: software channel init with chip type, debug mode, verify control interface (reg 13/14 probe and RAM 449 test) | KO `VINETIC_InitChMember` 0x3e00 |
| On first "phone init": `ProSLIC_Init_MultiBOM` (identify, patch load, verify, enable, general parameters, MADC cal, DC-DC up, calibrations) | KO `VINETIC_Basic_VoIPConf` 0x3bfc; LOG 295-313 |
| Then LB calibration or reload of saved results; PCM base configuration (6.2); analog line base configuration (DC feed preset 0, impedance, gains, linefeed forward active); signalling base configuration (tones, DTMF, caller ID); enable interrupts | KO 0x3c74..0x3d38 call order; LOG 314-322 |
| Ring frequency 20 Hz set afterwards | LOG 326-327 |
| Finally the application assigns TDM slots on the DSP side and moves TDM ownership to the CSS core | LOG 426-430 |

### 6.6 Versions

Stock module: "IFX SiLabs device driver 7.2.13.0", built Oct 15 2024,
author string "Penio Radev", description "Nuvoton device driver", proc
directory `driver/si32260-cpe` (KO modinfo and strings). Embedded ProSLIC
API version string not pinned down; it contains Si3228x support and
multi-BOM init, so it is newer than 7.3.2 in places, while its Si3226x
patch is the 2012 one. BSP: "SiLabs BSP driver 1.0.0", Aug 1 2024.

---

## 7. Open questions and risks

1. **Chip identity on V2.5A (highest priority).** Stock tables say
   2 x single-channel, family type 1 (Si3217x). "Chip Type 28" is from
   another unit's log. Settle with dmesg or the proc register dump before
   writing family-specific code. If the proc dump shows reg 0 with part
   bits and rev 2, it is a Si3217x rev C.
2. **Which general-configuration row is used** on this unit
   (`slic_dcdc_type=2` to row mapping is in closed `gs_ata`). The two
   Si3226x rows have opposite switch polarity. For Si3217x there is only
   the LCQCUK row in the module. Verify DCDC_SWDRV_POL (RAM 1553) and the
   DCDC_ANA_* values from a stock RAM dump before ever writing PD_DCDC.
3. **Patch-free operation of rev C is undocumented.** Nothing public says
   it is safe; the patch touches DC-DC related RAM. Treat as an
   experiment with the converter protected (4.2). The alternative is to
   load the patch, which is byte-identical to files in public vendor
   tarballs (4.3); that is a licensing decision for the user.
4. **PCM details not proven locally:** PCLK frequency and FSYNC shape
   produced by the DVF99 TDM block, whether the DSP side uses 16-bit
   linear (likely) or companded, the slot of the second device, and the
   exact meaning of count 0 relative to the FSYNC edge. Regs 11..15 from
   the stock dump answer the SLIC side; the TDM block needs its own
   register dump.
5. **Clock dependency.** The SLIC needs PCLK/FSYNC before and after
   reset. Under stock, TDM belongs to the CSS DSP. Once `gs_ata` and the
   stock modules are stopped, it is not known whether PCLK keeps running;
   if it stops, the SLIC loses its PLL (and the DC-DC controller its
   clock). Bring the TDM clocks up yourself first, then reset the SLIC.
6. **Stopping the stock driver with the converter running.** Unloading
   `drv_silabs` may or may not power the DC-DC down. Before taking over,
   put LINEFEED to open and run the power-down sequence (4, step 9), or
   pulse reset.
7. **MSTRSTAT / PLL-lock bit layout** is not in any public source I
   found; the public driver does not use it. Use the reg 13/14 readback
   probe instead.
8. **Bit-level meaning of several registers is inferred** from API usage
   (LINEFEED upper nibble, LCRRTP bit 0, IRQ/IRQ0 summary, RINGCON bits,
   ENHANCE bits other than 0, 2, 4). They are marked in the tables.
9. **Si3217x converter power-up and rev C helper stages** were located
   but not transcribed (4.6). Needed if item 1 confirms Si3217x.
10. **Stock runtime overrides** (ring current limit, ring-trip threshold,
    "VBAT tracking disabled", presets for DC feed, ringing and impedance)
    were not decoded from the binary. All are visible in a stock RAM dump,
    and the preset tables are in KO .data (`Si3226x_DCfeed_Presets` 0x1890,
    `Si3226x_Ring_Presets` 0x1ad0, `Si3226x_Impedance_Presets` 0x1310,
    `Si3217x_DCfeed_Presets` 0x6fc, `Si3217x_Ring_Presets` 0x87c,
    `Si3217x_Impedance_Presets` 0x1fc) with the public structure layouts.
11. **GPIO numbering** in HARDWARE.md drops the bank cell (6.1).
12. **Provenance of the public API files**: proprietary headers inside
    GPL tarballs. Facts and numbers are used here; code is not.
