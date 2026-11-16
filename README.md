# v90d: a dial-up server in a Grandstream HT802

v90d is replacement firmware for the Grandstream HT802 analogue telephone
adapter. It turns the box into a small dial-up internet provider: plug an
old computer's modem into one of the two phone ports, dial the number, and
the modem connects (up to V.90 / V.92 speeds) and gets a PPP session that is
routed out of the box's Ethernet port. No phone line, no SIP, no PC in
between.

It installs from the stock firmware's own upgrade page with one file, and
the stock firmware stays in the box, one click away.

## What it does

- Answers a modem on either phone port: V.92, V.90, V.34, V.32bis, V.22bis,
  V.22 and V.21, with V.42 error correction and V.42bis / V.44 compression.
- PPP server with users and passwords, MPPC compression for Windows
  callers, NAT to the network on the Ethernet port, DNS handed to the
  caller.
- Telephony the way an exchange does it: dial tone, tone or pulse dialling,
  ringing tone, busy, a number per port (North American or European tones).
- A service per port:
  - **Internet**: the plain connection.
  - **Archive**: web names are steered to Protoweb, so period browsers get
    period sites.
  - **Dreamcast**: what a Dreamcast's built-in modem needs: no compression,
    the DNS servers of the Dreamcast Live community, the packet fixes, the
    games' dead server addresses turned to today's, the games' ports let
    in. Optional switches (per port, off unless you turn them on): the
    console as a host of its own on the network (DHCP), port mapping on the
    router by UPnP, reporting to "Dreamcast Now", and a VPN for the IWANGO
    games (below).
  - **Saturn**: two consoles that telephone each other (NetLink), each
    dialling into its own box, joined over the network. Dial `0` to wait,
    `#a*b*c*d#` to call the player at that address. An optional matchmaking
    mode (off by default) pairs players who dial the same game through a
    match server and works through home routers. Speaks the DreamPi
    tunnel's protocol, so the other side can be a DreamPi.

  The two console services are written from the DreamPi project's
  published behaviour and tested against simulations; neither has been
  tried with a real console yet. The IWANGO games (Sega Tetris, Aero
  Dancing, Golf Shiyouyo 2, Daytona USA) reach their servers through an
  OpenVPN tunnel: turn the switch on and paste your own `.ovpn` profile on
  the Consoles page. v90d ships no profile or keys of its own; bring one
  from whoever runs that network. Not done: DreamPi's closed helper programs.
- A web interface ("Nestaliga") for all of it: dashboard, line, modem,
  users, network, system, firmware and calls, with a Consoles page (the
  Dreamcast switches and the Saturn matchmaking switch) when a port uses one
  of those services. The pages follow the box: both phone ports on an HT802,
  a single port on a one-port box.

## What it is made of

Linux, busybox and a C library are used as they are. Everything else in the
image is written here from the ITU-T recommendations and the RFCs: the board
support and drivers for the HT802's chips, the telephony, the modem itself
(the signal processing of every modulation above, in fixed-point C99), the
link layers, the PPP server, the web interface, the loader and the build.
The one other outside part is OpenVPN (with mbed TLS), built unchanged from
source for the optional IWANGO tunnel. There is no Grandstream code or
binary in the image and no closed blob.

`DESIGN.md` says how it is built and why, `HARDWARE.md` what was learned
about the box, `PROGRESS.md` what was measured and when, with the test
output.

## Status

Tried on one unit: HT802 hardware V2.5A, part number 9610003725A, coming
from stock firmware 1.0.65. The loader refuses to start on a board whose
part number it does not know and hands the box back to the stock firmware,
so an untried revision should end up where it started; that path has been
tested on models only. The HT801 and other HT802 revisions are expected to
work but are not yet tested; see [BOARDS.md](BOARDS.md).

Tested with a Conexant USB modem on every modulation in the list except
V.92, from Linux pppd and from Dial-Up Networking in Windows 98 SE and
Windows XP (V.90 with MPPC compression in both). V.92 is written and passes
in simulation, but it is off by default (`v92 = on` to offer it) and has
not yet been proven on a real V.92 call. Not tested yet: V.92 on hardware,
other modem brands, two calls at the same time, a real Dreamcast or Saturn,
Windows 95 and Windows Me.

## Installing

Download `v90d-ht802fw.bin` from the releases page and see
[FLASHING.md](FLASHING.md). In short: log in to the stock web page, wait two
minutes after the box has started, upload the file on the firmware upload
page, wait three minutes. The box comes back on the same address with the
v90d pages (user `admin`, password `admin`; change it). An existing v90d is
upgraded the same way from its own Firmware page, and the settings are kept.

## Building

A Linux PC with the usual build tools (gcc, make, python3, curl, bc, flex,
bison) is enough.

```
sh fw/setup.sh          # fetches the cross toolchain, Linux, busybox, OpenVPN and mbed TLS (checked by SHA-256) and builds them
make -C fw release      # fw/build/release/v90d-ht802fw.bin
```

Tests (no box needed):

```
make check              # everything on the PC
make check-arm          # the same code built for the box, under qemu-arm (needs qemu-user-static)
make -C fw test-release # the firmware file, the flash tools and the loader against models of the box
```

## Layout

| directory | what |
|---|---|
| `modem/`, `v32/`, `v34/`, `v90/`, `v92/` | the answering modem |
| `v42/` | V.42, V.42bis, V.44 |
| `ppp/` | the PPP server |
| `tel/` | tones, dialling, the call's life |
| `dce/` | modem, link layer and PPP tied into one call |
| `srv/` | the daemon: calls, network, NAT, the per-port services |
| `cfg/` | settings, status and the web interface |
| `arch/` | the Archive service's helpers |
| `fw/` | the firmware: kernel board support and drivers, loader, flash tools, image and release build |
| `sim/`, `test/`, `rig/` | line simulator, tests, real-modem test rigs for a PC |
| `live/` | the bench scripts used for the measurements in `PROGRESS.md` |
| `refs/` | notes on the hardware, as facts |

## Licence

GPL-2.0-only, see [LICENSE](LICENSE). Contributions are welcome, see
[CONTRIBUTING.md](CONTRIBUTING.md).

This project is not affiliated with Grandstream. Flashing third-party
firmware is at your own risk.
