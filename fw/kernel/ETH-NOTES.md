# OBi200 Ethernet: UniMAC AV controller and the embedded switch (ESU)

Reconstructed from the stock Linux 2.6.30 kernel (RAM dump `ktext.bin`, its
`/proc/kallsyms`), the driver functions `uniAV_*` (c0235490..c0237800), the
HAL `mvEth*` (c02377f4..c0239264), the switch glue `mv_sw_init` (c00165d0),
`swdev_smi_read/write` (c01f3560) and the Marvell QuarterDeck library
(`qd*`, `gprt*`, `gsys*`, `hw*`). The dump is of a running system, so the
driver's globals (`G` = c056ef30) and the switch's QD device struct
(c0502a80) hold live values; those are quoted where useful.

Marks: **[V]** read off the disassembly or the live data, **[I]** inferred
from how the code uses it, **[G]** guess.

Driver: `drivers/net/ethernet/marvell/ffxav_eth.c`. DT node in
`arch/arm/boot/dts/obihai/ffxav-obi200.dts`.

## Resources

| what | where | |
|---|---|---|
| controller "unimacAV.0" | 0x80008000-0x80008fff, IRQ 9 ("uniAV_Eth") | [V] platform resources at c04b0a48 |
| switch "esu.0" | 0x8000e000-0x8000efff, IRQ 10 ("mv_avb", not needed) | [V] c04b0a08 |
| SMU SRAM | 0x88000000, 8 KiB (`ffxav_get_smu`) | [V] |

The stock driver maps the controller with `ffxav_ioremap` (virtual
0xfcc08000) and always uses that fixed mapping in the data path. All
accesses are 32-bit.

## Controller register map (offsets from 0x80008000)

Transmit side at 0x000, receive side at 0x400, same layout. Ten queues per
direction, `q` = 0..9, stride 0x20.

| off | name (ours) | use | |
|---|---|---|---|
| 0x000 / 0x400 | TX_GCFG / RX_GCFG | written 0x80000c70 at init | [V] meaning [G] |
| 0x004 / 0x404 | TX_QMASK / RX_QMASK | written 0x3ff at init (all 10 queues) | [V] meaning [G] |
| 0x008 / 0x408 | TX_DONE / RX_DONE | bits 31:16 = number of finished frames not yet acknowledged (non-zero = work), bits 3:0 = the queue they are on. Not cleared by reading (the stock reads it twice per frame) | [V] |
| 0x00c / 0x40c | TX_IND / RX_IND | indirect access: bit 31 busy (poll until 0), bits 30:24 command (0x02 read, 0x03 write), 23:16 register (0x10 = queue enable mask), 15:0 data. Read: write 0x82100000, poll, data in 15:0. Write: 0x83100000 \| data, poll | [V] |
| 0x080+0x20q / 0x480+0x20q | TXQ_CFG / RXQ_CFG | init: TX 0x2160, RX (1536 << 16) \| 0x2160 (31:16 = buffer size); at start bits 10:8 = ring size code (16 -> 0, 32 -> 1, ... 2048 -> 7) | [V] other bits [G] |
| 0x084+0x20q / 0x484+0x20q | TXQ_PROD / RXQ_PROD | producer index, 11 bits, absolute (stock: `((read & 0x7ff) + n) & (size-1)`, written back). TX: +n after queuing n descriptors. RX: +1 per buffer given | [V] |
| 0x088+0x20q / 0x488+0x20q | TXQ_ACK / RXQ_ACK | write 0x8001 once per finished frame handled (decrements the count in *_DONE) | [V] write, effect [I] |
| 0x08c+0x20q / 0x48c+0x20q | TXQ_BASE / RXQ_BASE | ring bus address (32-byte aligned) | [V] |
| 0x090 (+0x20q?) | | low 11 bits polled non-zero by `mvEthEcho` before sending: free space or hardware index | [I] |
| 0x800 | TX_COAL | bits 22:7 TX coalescing; init `(v & 0x7fff80) \| 0x20000` | [V] |
| 0x804 | RX_MODE | bits 2:0 header mode (stock 1), 22:7 RX coalescing; init `(v & 0x7fff80) \| mode` | [V] |
| 0x818 | INT_CAUSE | read, then write the value back to clear | [V] write-back, W1C [I] |
| 0x81c | INT_MASK | bit 2 TX done, bit 3 RX, bit 10 RX "more" (see below) | [V] |
| 0x840 | RX_CHAN | bit 31 busy (poll), bits 9:0 queue enables | [V] |
| 0x844 | TX_CHAN | same for transmit | [V] |
| 0x880, 0x884 | | class shaper (`mvEthTxClassShaper`) | [V] unused |
| 0x888+4r | | ring shaper (`mvEthTxRingShaper`, r = 2..9) | [V] unused |
| 0x000 / 0x400 bit 10, 0x010 / 0x410, 0x014 / 0x414 | | `mvEthRingScheduling`: GCFG bit 10 (strict vs. weighted [I]), per-queue 4-bit weights, two more fields | [V] unused |
| 0xa80, 0xa84, 0xa88.. | | DA to ring map (`mvEthDA2RingMap`), AVB stream steering | [V] unused |

No MAC address register exists in the controller: the stock never programs
one. Address filtering is done in the switch (static ATU entries added by
`mv_add_static_mac`, flooding to the CPU port set by `gprtSetEgressFlood`).
[V]

There is no PHY, link or speed handling in the controller driver:
`uniAV_eth_get_link` returns 1, `open` calls `netif_carrier_on`
unconditionally. [V]

### Channel enable / disable (mvEthChannelEnable / Disable) [V]

`ch` 0 = transmit (TX_IND, TX_CHAN), 1 = receive (RX_IND, RX_CHAN).

Enable(ch, mask): indirect read of 0x10, indirect write `old | mask`; then
poll CHAN, write `0x80000000 | mask | (old & 0x3ff)`, poll.

Disable(ch, mask): poll CHAN, write `(old & ~mask) | 0x80000000`, poll; then
indirect read 0x10, indirect write `old & ~mask & 0xffff`.

## Descriptors [V]

16 bytes, little-endian, 32-byte-aligned ring of 2^n entries (16..2048),
no ownership bit, no wrap bit (the ring size code in Q_CFG gives the wrap).

| off | size | TX | RX |
|---|---|---|---|
| +0 | u16 | byte count of this buffer | byte count received: Marvell header + frame + FCS |
| +2 | u16 | command: 0x20 first, 0x10 last, 0x08 (set together with "last", interrupt or CRC [G]); a one-buffer frame is 0x38. 0x1000 with +4 = AVB launch time [I] | status: 0x20 first, 0x10 last (stock requires both), bits 2:1 error: 4 = bad FCS (counted as rx_crc_errors), 2 and 6 other (rx_errors) |
| +4 | u32 | AVB parameter, only with 0x1000 | not used by the stock [V]; timestamp [G] |
| +8 | u32 | buffer bus address | buffer bus address |
| +12 | u32 | software: the skb (last descriptor) or the number of descriptors of the frame (first descriptor of a multi-buffer frame) | software: the skb |

The stock only writes +8 and +12 when re-arming an RX descriptor; length and
status are left for the hardware to overwrite. Buffers: RX 1536 bytes (the
size is also in RXQ_CFG), the stock reserves 512 bytes of headroom in each
skb before the buffer. Bus address = physical (virtual + 0x40000000 on the
stock's 0xc0000000 mapping).

Ownership is by index registers only: the hardware owns RX descriptors from
its consumer position up to RXQ_PROD; finished frames are reported by
count in RX_DONE and returned with one RXQ_ACK write each. Same for TX
with TX_DONE / TXQ_ACK. An RX ring holds size-1 buffers (the stock's "full"
test is `next(refill) == current`). [V]

## Start / stop sequence [V]

Probe (`uniAV_eth_load`, `uniAV_eth_init`, `mvEthInit`):
1. Reset (`mvEthReset`): Disable(0, 0x3ff), Disable(1, 0x3ff); write 0 to
   TXQ_PROD(q) and RXQ_PROD(q) for all q.
2. TX_COAL, RX_MODE (header mode 1), RX_GCFG = TX_GCFG = 0x80000c70,
   RX_QMASK = TX_QMASK = 0x3ff, RXQ_CFG(q) = 0x06002160, TXQ_CFG(q) = 0x2160.
3. Rings: 64 descriptors per queue in header mode 1 (all 10 TX, all 10 RX);
   queues 0-3 in the SMU SRAM, 4-9 in `dma_alloc_coherent` DRAM (live: TX
   DRAM 0x0307a000, RX DRAM 0x0306e000, SMU fully used). If the SMU is
   taken, everything goes to DRAM (code path exists, not seen live).
   Header mode 4 uses other sizes (RX 32, TX 32/32/16..) and all queues in SRAM.

Open (`uniAV_eth_open`, first opener only):
1. Give every RX queue size-1 buffers (`mvEthInitRxBuffer`: desc +8, +12,
   RXQ_PROD + 1 each), before the ring base is written.
2. Ack INT_CAUSE (read, write back); INT_MASK = 0x0c.
3. `mvEthStart`: for each TX queue, then each RX queue: Q_BASE = ring bus
   address; Q_CFG bits 10:8 cleared, then set to the size code;
   Enable(ch, 1 << q).

Stop (`uniAV_eth_stop`, last closer): INT_MASK = 0, ack INT_CAUSE;
`mvEthStop`: per queue Disable(ch, 1 << q), Q_BASE = 0, Q_PROD = 0; free
the buffers; rewind software pointers.

Transmit (`uniAV_eth_start_xmit`, queue 0 for all normal traffic): push 2
bytes, write the header, flush the cache over the frame, fill the
descriptor(s), then TXQ_PROD += n. TX done (tasklet): while TX_DONE[31:16]:
queue from [3:0], walk to the descriptor with 0x10, write 0x8001 to
TXQ_ACK(q), free the skb.

Receive (tasklet, `uniAV_eth_receive_multi_queues`, budget 64, 8 in mode
4): while RX_DONE[31:16]: queue from [3:0]; descriptor at the queue's
current position; if status has first and last and no error: take a new
skb, advance, write 0x8001 to RXQ_ACK(q), `skb_put(len - 4)` (FCS),
`skb_pull(2)` (Marvell header), deliver, re-arm the refill slot with the
new buffer and RXQ_PROD + 1. (The stock's error path neither acks nor
advances; ours does both and re-arms the same buffer.)

Interrupt handler [V]: read cause and mask; if cause & 4: mask &= ~4, TX
tasklet; if cause & 0x408: mask &= ~0x408, RX tasklet; write the cause
back. The RX tasklet sets mask bit 3 again when it finished under budget,
else bit 10 (so bit 10 seems to mean "frames still pending" [I]). The TX
tasklet sets bit 2 again. Our driver uses NAPI and only ever unmasks 0x0c.

## The Marvell header and the "gateway interfaces"

Header mode 1 (RX_MODE[2:0]) is set at probe and never changed by the
stock's application; `unicfg -he/-hd` would change it (1..3, 4 is the
multi-queue AVB mode that also re-sizes the rings and resets). [V]

Every frame in both directions carries 2 bytes in front of the destination
address, regardless of the interface: [V]

* TX: the driver pushes 2 bytes = the interface's 16-bit word at
  priv+26, i.e. byte 0 = `vid << 4`, byte 1 = `portVec` from
  `unicfg -gwadd <if> <vid> <mac> <portVec>`. For eth1 (`0 ... 0x2f`):
  bytes `00 2f`. For `unm`: `00 00`. In Marvell header terms (big-endian
  16 bits): DBNum = 15:12, VLANTable override = low bits; 0x2f = ports
  0, 1, 2, 3, 5 (the LAN jack must be among 0..3) [I from the QD naming and
  "-gwadd LAN 2 ... 0x1e = ports 1~4" in unicfg's help].
* RX: buffer = 2-byte header, frame, 4-byte FCS. The stock picks the
  interface by `(byte0 >> 4) & 7` (DBNum of the frame) as the index of the
  gateway interface table, when at least one gateway interface exists;
  otherwise all go to `unm`. No other per-frame header. [V]
* The gateway interface: just another net_device on the same rings,
  with its own MAC (added as a static ATU entry), its own 2-byte TX header,
  receiving the frames whose DBNum equals its `vid`. `unicfg -gwadd` is
  ioctl 0x80208a05 on /dev/unicfg (char 138) taking `{char name[17]; u8
  mac[6]; u8 pad; u32 vid; u32 portVec}`. [V]
* "etype mode" (`unicfg -ep`, off by default and in the box's boot): for
  frames whose type is 0x9000 (EtherType DSA), copy the inner type over it;
  the 8 tag bytes stay. Only useful for PTP; ignored. [V]
* "multi-queue receive": all 10 RX queues are serviced; the queue comes
  from RX_DONE[3:0]. How the hardware chooses the queue (priority, DA map)
  is not known; normal traffic on the live system [G] lands in queue 0
  or 1. Our driver arms all 10 (queue 0 with 64 buffers, 1-9 with 16).

Our driver: one net_device, TX header from `marvell,tx-header` (default
0x002f, eth1's), RX header stripped whatever its DBNum, plus an EDSA strip
(type 0x9000 after the source address: 8 bytes removed) in case the switch
tags trapped frames.

## Switch (ESU)

Product ID (port register 3, SMI device 8): 0xc00x, the QD library's
"0xC00" device: 7 ports, CPU port 5, valid ports 0x7f, old (88E6060-style)
register addressing, `devName1` bit 0 family. [V live QD struct: deviceId
0xc00, cpuPortNum 5, baseRegAddr 0, numOfPorts 7] Not a chip that the
kernel's mv88e6xxx DSA driver knows (no 0xc00 in its tables), and not on
an MDIO bus: no DSA.

Access [V] (`swdev_smi_read/write`): memory-mapped, 32-bit, data in bits
15:0: `0x8000e000 + (smiAddr << 7) + (reg << 2)`.

| SMI device | what | address |
|---|---|---|
| 0..6 | PHY of port n | 0x8000e000 + 0x80 n |
| 7 | Global 2 | 0x8000e380 |
| 8..14 | port n registers | 0x8000e400 + 0x80 n |
| 15 | Global 1 | 0x8000e780 |

Port registers used (from the QD functions for this device) [V]:
reg 0 status, 3 ID, 4 Port Control (1:0 STP state, 3 = forwarding; 3:2
egress floods: 0 none unknown, 2 unknown multicast only (allmulti), 3 all
(promisc); 9:8 frame mode, 3 = EtherType DSA; bit 11 Marvell header),
6 port-based VLAN, 7 PVID, 9 egress rate (7:0 decrement, 11:8 frame
overhead), 10 egress rate 2 (15:14 limit/count mode, 11:0 rate), 15
port EType. Global 1 reg 0x1a bits 7:4 CPU destination; Global 2 reg 5
bit 12 flood broadcast.

What `mv_sw_init` does (stock kernel at boot) [V]: qdLoadDriver (all
ports forwarding, CPU destination = 5, PIRL init); PHY register 0 = 0xb000
(reset, autoneg, 100) on ports 1..6; CPU port Marvell header on; flood
broadcast on; all ports disabled; QoS and AVB setup per port (priority
maps, PVID 1, scheduling); reserved multicast 01:80:c2:00:00:00 and :0e
to the CPU; CPU port frame mode 3 (EtherType DSA) and EType 0x9000; all
ports forwarding; PHY interrupts on; switch IRQ 10.

The two `swcfg` writes [V]: `swcfg -sw 0 5 9 0x28` and `-sw 0 5 0xa 0xb0c8`
are port 5 (the CPU port) register 9 = 0x0028 and register 10 = 0xb0c8:
the egress rate limiter towards the CPU: decrement 40, rate field 200,
count mode 2 (layer-2 bytes), bits 13:12 set [?]. With `grcSetEgressRate`'s
formula (rate = 8000000 dec / (32 field) kbit/s) that is 50 Mbit/s [I]:
it protects the CPU, nothing to do with forwarding.

Minimum setup for LAN <-> CPU (what our driver does, `switch_setup=1`):
check the ID; Global 1 0x1a[7:4] = 5; Global 2 5 bit 12 = 1; port 5
control: header on, frame mode 3, floods 3 (everything unknown to the CPU,
since the controller has no address filter and we add no ATU entries);
port 5 EType 0x9000; port 5 regs 9/10 as the maker's (`cpu_rate_limit`);
every port's STP state forwarding. PHYs untouched; the port-based VLANs
left as found (reset default: all ports see each other [G]).

Which port has the jack: not known; one of 0..3 [I from 0x2f]. The probe
dump prints every port's status and PHY BMSR: link up shows there.

## State when RAM-booted from the stock kernel vs. a cold boot

RAM-boot (fw/obi/ramboot): `ifconfig eth1 down; ifconfig unm down` runs
first, the last close runs `uniAV_eth_stop`: INT_MASK 0, queues disabled,
bases and indices 0. [V] The rest stays as the stock left it: header
mode 1, queue configs, coalescing, and the whole switch setup above
including ATU static entries for 00:00:4d:52:56:4c (unm), the unit MAC and
eth1's multicast list, CPU port flood mode 2 (eth1 was `allmulti`), the
rate limit. Our probe stops and re-programs everything it relies on anyway.

Cold boot from U-Boot: nothing in the stock driver, HAL or switch glue
writes clock, reset or pin registers (the 0x8000f000 system block is only
read by `_ffxav_init` for clock rates) [V for these code paths; the rest of
the kernel not searched exhaustively]. So either the controller and the
ESU run from reset, or U-Boot enables them. Unknown [G]. The switch's
reset defaults (no header on port 5, frame mode normal, maybe ports
disabled) are fixed by our switch setup; the PHY reset the stock does on
ports 1..6 is not done by us.

The SMU SRAM (0x88000000) is free in our kernel (no audio driver);
`sram_rings=1` puts our rings there as the stock does for queues 0-3.
