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
   device tree.
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
