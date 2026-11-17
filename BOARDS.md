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
