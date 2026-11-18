# Supported boards

v90d is a Grandstream HT80x firmware. This lists the boards it is known or
expected to run on, and what "supported" means for each.

v90d will not start on a board it does not recognise, **by design**. The
loader reads the board's part number from the stock `factory` partition and
runs v90d only if that number is in its built-in list; on any other board it
hands control straight back to the stock firmware. So an unlisted board is
never left running v90d and cannot be bricked by trying one: it simply boots
stock as before.

## Boards

| Board | Hardware | Part number | SoC | FXS ports | SLIC | Status |
|---|---|---|---|---|---|---|
| HT802 | V2.5A | `9610003725A` | DSPG DVF9918 | 2 | 2 × Si3217x | **Tested** |
| HT802 | other V2.x | not yet known | DVF9918 (expected) | 2 | 2 × Si3217x (expected) | **Expected, untested** |
| HT801 | V1.0A | `9611003610A` | DVF99 (same device tree) | 1 | 1 × Si3217x (factory data) | **Accepted by the loader, untested** |
| HT801 | V1.1C | `9610003611C` | DVF99 (same device tree) | 1 | 1 × Si3217x (factory data) | **Accepted by the loader, untested** |
| HT801 | V2.x | not yet known | DVF9918 (expected) | 1 | 1 × Si3217x (expected) | **Expected, untested** |
| HT802V2 | V2.0A | `9610009420A` | not confirmed | 2 | not confirmed | **Signed firmware; under investigation** |

## What the status words mean

- **Tested** — v90d has been installed on a real unit of this board and a
  dial-up call made through it. This is the reference unit, from stock
  firmware 1.0.65; its facts are in `HARDWARE.md`.
- **Expected, untested** — Grandstream ships one firmware for the HT801 and
  HT802 (the stock image branches on the model for the number of ports
  only), and the stock board tables give these boards the same DSPG DVF9918
  SoC and the same Si3217x SLIC family as the tested unit. So the code is
  expected to work: the modem, PPP, web interface, loader and flash tooling
  are all SoC-level and unchanged, and a single FXS port is already a
  first-class mode in the daemon, the settings and the web pages. But no
  such unit has been tried, and **the loader does not yet carry its part
  number, so it will not start there** until a board is added (below).

- **Accepted by the loader, untested**: the part number is in the loader's
  list, so v90d starts on this board, and the line driver brings up one port
  when no second SLIC answers. Nobody has run it there. See the HT801 section
  below for what is known and what can go wrong.

- **Signed firmware; under investigation**: the stock upgrader on this board
  refuses v90d because the image is not signed, so the normal install path is
  closed. Whether v90d can still be booted another way is an open question
  (below), not yet decided either way.

## The HT802V2 (signed firmware)

The **HT802V2** is a newer product than the HT802 this firmware was built on,
not a hardware revision of it. It is reported (issue #1) to run a signed
firmware generation: stock firmware 1.0.15.x, dropbear SSH into a restricted
"HT802V2 Command Shell" with no shell escape, lighttpd, and a `status` that
reports `Certificate Type: ECDSA+SHA384` and `SecureStatus: 3`.

**The stock upgrader will not install v90d on it.** The v90d install path
depends on the stock upgrader accepting an unsigned, repacked image: on the
generation this project targets, the flasher checks only the container magic,
a 16-bit checksum and version/generation/hardware compatibility, with no
public-key signature (`HARDWARE.md`, "Firmware image format"). The HT802V2
adds exactly that missing check:

- The stock container format and the 32-character header key are unchanged,
  and the four partition **headers** still decrypt, so the upgrader still
  recognises our file (it reaches "Upgrading").
- But there is a ~256-octet high-entropy block in front of the container
  magic (a signature/certificate block), and the partition **payloads** no
  longer decrypt with the body-key derivation of the older format. The
  upgrader then fails a cryptographic check and reports "Invalid".
- `mkgsfw.py` cannot produce a block this unit accepts: there is no key or
  derivation in the public image that would let us sign for it. Uploading a
  repacked image is a dead end.

**Whether v90d can run at all is still open, and turns on one question: does
the signed U-Boot verify the kernel?** Secure boot is a chain. The bootrom
verifies U-Boot; U-Boot may or may not verify the kernel it boots. On the
tested HT802 (V2.5A), U-Boot (`gsboot`) checks only the magic word and a
checksum, with no signature (`HARDWARE.md`, "Bootloader"), and v90d already
installs by leaving the stock U-Boot in place and replacing only the core
(kernel), base and prog (userspace) partitions. If the HT802V2's signed
U-Boot is the same in that respect, v90d's kernel and userspace could be
written **past the upgrader** (over the serial console with U-Boot's own
`sf`/`qspi` write, or with an external flash programmer, neither of which
checks a signature) and the genuine signed U-Boot would boot them, even with
bootrom secure boot fully enabled. If instead the V2 tightened U-Boot to
verify the kernel, that route is closed and only a signing key or a bootrom
bug would open it.

The two things that decide it:

1. **A UART boot log** from a V2. It shows whether the signature is enforced
   at the upgrader only or also at `bootm`, and whether autoboot is
   interruptible.
2. **A dump of the `uboot` partition** (~256 KB; `sf read` / `qspi read` from
   the U-Boot prompt). Disassembling `gsboot` shows the kernel check directly,
   the same way it was read on the non-V2 unit. A full flash dump is not
   needed and would include the unit's `factory` data (MAC/serial), which is
   best kept private.

The SoC and SLIC of this board are also unconfirmed, since there is no root
shell to read `/proc`.

## The HT801, from two flash dumps

Two complete flash dumps of HT801 units are public
([crvallance/Grandstream-hax](https://github.com/crvallance/Grandstream-hax)).
Read as facts, compared with the tested HT802:

- Part numbers `9611003610A` and `9610003611C` (hardware V1.0A and V1.1C).
  In these numbers `36` is the HT801, `37` the HT802, and the last three
  characters the hardware version.
- The two HT801 device trees are identical to each other, and differ from
  the HT802's only in the second SLIC: one chip select (the HT802's port 1),
  one reset line, one interrupt line.
- The flash layout is the HT802's, partition for partition.
- The factory data gives the same SLIC family, converter type and
  protection setting as the HT802, with one SLIC device instead of two.

What v90d does with that: the loader accepts both part numbers, and the line
driver asks the second chip select whether a SLIC is there before it makes
the second port. On an HT801 the daemon then runs with one line and the web
pages show one port. On the tested HT802 this was checked: both ports found,
a call made through port 2.

What is not known, and matters before anyone flashes one:

1. **Ethernet.** The firmware does not name the PHY: the device tree says
   only RMII at 100 Mb (the same words in all three), the factory data has
   no record that differs for it, and the stock kernel asks the chip for
   its identity when it runs. V1.x boards are reported to carry a KSZ8081,
   the tested V2.5A unit a YT8512H. That should not matter to v90d: the
   box's own boot loader, which v90d leaves in place, resets the PHY and
   sets its clock on every start, and our kernel then talks to it with the
   generic driver at MDIO address 0, which is where the vendor's published
   source puts the KSZ8081. It is not proven until seen on a unit, so on
   these boards a start is confirmed only once the network port has
   received something: a box whose Ethernet does not work goes back to the
   stock firmware by itself after three starts. (So does one that is
   started three times in a row with no cable in.)
2. **SLIC revision.** The driver starts a Si3217x of revision C only. Another
   revision is refused cleanly (no battery voltage, a line in the log), and
   the port stays dead.
3. **V2.x HT801 units** have a part number that is not in the list yet.

Whoever can should make the first start on an HT801 from RAM
(`fw/tools/ramboot.sh`, which writes nothing) with a serial console
attached. Installing from the stock web page is expected to be safe as
well, for the reason in point 1, with the network cable plugged in.

## Adding a board

The code that is specific to a board is small and in three places, so
bringing up a compatible board is mostly reading values off a unit, not
writing new code:

1. **Part number** — read it from the unit (the label, or flash offset
   `0x4020` in the `factory` partition) and add it to `LD_BOARDS` in
   `fw/loader/boot.h`.
2. **SLIC wiring** — the reset, chip-select and interrupt GPIOs and the pin
   mux, in `fw/kernel/arch/arm/boot/dts/dspg/dvf99-ht802.dts` (and the
   `v90line` driver). On the HT801 only one SLIC is populated; its pins are
   likely the HT802's port-1 SLIC, to be confirmed from the board's own
   device tree. The driver finds out by itself whether the second SLIC is
   there.
3. **Flash layout** — the set A/B core and base offsets in
   `fw/loader/boot.h`. The HT801 and HT802 use the same 16 MiB flash and the
   same stock firmware, so these are almost certainly identical; confirm
   with `/proc/mtd` on the unit.

Then, before ever writing flash, RAM-boot the image on the unit with
`fw/tools/ramboot.sh` (it writes nothing) and confirm the FXS port(s), the
SLIC DC-DC ringing supply (`fw/tools/slicpoke`) and the flash map. Only once
the no-serial recovery path is proven on the board does it move from
"expected" to "tested". See `HARDWARE.md` for the HT802 facts and the
starting points for the HT801 and other HT802 revisions.

## The one electrical risk

The Si3217x needs the right DC-DC converter configuration loaded at init
(the stock board table carries a DC-DC type per model). If a board's ringing
supply differs from the tested HT802's, those init values need matching
before ringing is trusted. This is the same risk that was cleared on the
HT802 and is checkable on the bench without writing flash.
