# v90d on the Obihai OBi200

The OBi200 (one telephone port) runs v90d with the maker's kernel and the
community firmware's system around it. Only the box's `/obi` part of the flash
(the maker's application) is replaced.

Each release carries **`v90d-obi200.fw`**, ready to install. It is built by
the release CI from the community firmware file arrynrob13 (fetched and
checked by `getbase.sh`): it is that whole file with our `/obi` in place of
the maker's, the maker's telephony, LED and watchdog kernel modules copied
into it. You can also build it yourself, below.

Verified on a unit (OBi200 hardware 1.4, community firmware arrynrob13, build
8680EX):

- a U.S. Robotics 56K (5.4.62) calls in at **V.92 54 666 / 33 333 bit/s**, LAPM
  with V.42bis, and gets PPP and the Internet through the box's NAT
- updates through v90d's own Firmware page, and back again (see below)
- the power light is steady as under the maker's application; the phone light
  is on while the telephone is off hook

Not tried: the OBi202 (two ports), other OBi models, other base firmware
files. The phone light's position (light 1 of the maker's LED module) is
inferred, not yet seen on the unit.

## What you need

- An OBi200 that accepts firmware the maker did not sign. `v90d-obi200.fw`
  is the community firmware arrynrob13 (obifirmware.com) with v90d in it, so
  it goes on wherever that firmware goes, by the same route: follow the
  community install instructions and upload `v90d-obi200.fw` where they say
  arrynrob13. In short: stock before build 5853, directly; 5853 to 6258,
  after `bypass-uboot-mod`; 6259 and later, after stock 5921EX and then
  `bypass-uboot-mod` (that one straight from 6259 or later ends in recovery
  mode). A box already on the community firmware is ready; that is the path
  tested here, the stock routes have not been tried with v90d.
- The community firmware file (`OBi2xx-3-2-2-8680EX-arrynrob13.fw`, md5
  `b438e5161220e1e7c161e005de1d6bb6`): it is your way back.

## Build it yourself (optional)

With the v90d sources and the toolchain set up (`fw/setup.sh`), `mksquashfs`
and `unsquashfs` (squashfs-tools) and Python 3:

    make -C fw/obi image BASE=$(sh fw/obi/getbase.sh)

gives `fw/obi/build/v90d-obi200.fw`, the same file as the release's: the
whole firmware file (about 12 MB), identical to the base except for `/obi`.
`make -C fw/obi test BASE=...` also runs the flash tool's tests.

## Install

On the OBi's own web page (System Management, Device Update), upload
`v90d-obi200.fw` as you would any firmware. The box writes it and restarts
into v90d, about three minutes. It then answers at the address it gets by
DHCP, with v90d's pages (user `admin`, password `admin` until you change it
on the Settings page). The telephone port is v90d's line; the settings live in
the box's writable flash (`/scratch/v90d`) and survive updates.

SSH (dropbear, root) keeps working: it belongs to the community firmware's
system, not to `/obi`.

## Update

On v90d's **Firmware** page, upload a newer `v90d-obi200.fw` (from a
release, or built the same way). The page checks the whole file, writes only what differs from the flash
(normally `/obi` alone), and restarts the box.

## Going back

Upload the community firmware file itself (BASE) on v90d's Firmware page. It
differs from the flash in `/obi` only, so that is all that is written, and the
box restarts into the maker's application with its old settings.

If the power goes while the flash is written, or something else leaves the box
unable to start, it comes up in the maker's **recovery mode**: a plain page
titled "OBi202 Firmware Update" at the box's address, which takes any firmware
file (BASE, or v90d's). v90d never writes the boot loader or the recovery
images, so that mode is always there.

## How it works

| piece | what it is |
| --- | --- |
| `obi` | the launcher, run by the system's `/etc/rc` at boot: loads the maker's modules, makes the LAN port, starts the line and the daemon, restarts the box when the Firmware page or the Restart button asks |
| `obiline.c` | the telephone line in user space: SLIC bring-up (fw/drv/slic.c, patch-free), the 10 ms audio loop, the hook, the lights; serves the daemon on `/var/run/v90line` (`seq:` in srv/line.h) |
| `obinv.c` | the Firmware page's flash tool on this box (the HT80x has fw/tools/v90nv) |
| `compat26.c` | `rename` and `accept4` for Linux 2.6.30, older than our C library expects |
| `mkobifw.py` | the firmware file builder; the file format is in its header |
| `udhcpc.script` | the LAN address by DHCP |

The daemon itself is the same as on the HT80x (`make -C srv v90d-obi`), built
with `compat26.c` and a 16 MiB Firmware page upload. Where this kernel lacks
something, the daemon falls back by itself: no TUN driver, so the PPP link's
network device is the kernel's own PPP unit over a pseudo-terminal
(srv/tun.c); no nftables, so NAT goes through the system's `iptables`
(srv/nat.c).

## Facts about the box

Read off the unit (the maker's binaries were studied, not used in ours).

**Flash** (16 MiB SPI, `/dev/mtd6` is all of it, 64 KiB sectors): U-Boot
0x0, factory block "OB100 UNIT INFO" 0x40000 (the unit's MAC at 0x40100),
kernel 0x50000, recovery kernel 0x2D0000, **header of the last firmware file
written 0x470000** (the boot checks the partitions against it: a firmware
file that does not carry every part leaves the box in recovery mode), root
file system 0x480000, `/scratch` (jffs2) 0xC0000, `/obi` 0xC00000 (0x240000),
recovery root file system 0xE40000.

**Firmware file:** see `mkobifw.py`. The update page checks a keyed MD5 over
the file's first 0x400 octets and over each part; the key is a string in the
maker's application, which the builder finds in BASE (it is not kept here).

**Telephony module** (`ffxav_mp.ko`), ioctls on `/dev/fxs0` (channel 0, the
jack) and `/dev/fxs1`:

| ioctl | argument | what |
| --- | --- | --- |
| `0xc00171b2` | `{u8 reg; u8 pad[3]; u8 value}` | read a SLIC register |
| `0x400471b3` | `{u8 reg; u8 pad[3]; u8 value}` | write a SLIC register |
| `0x71b6` | 0 / 1 | the SLIC's reset line: held / let go. The SLIC is held in reset from boot until the application lets it go |
| `0x7191`, `0x718a` | none | start the channel's TDM (the maker's application issues both) |
| `0x718b` | none | stop it |

The SLIC is a Si3226x rev C (ID 0xC3, LCQCUK converter); channel 1 is not
wired on the OBi200. `/dev/pcm` carries both channels: each read and write
is 320 octets, 80 16-bit samples of channel 0 then 80 of channel 1, every
10 ms. Open both `/dev/fxsN` and start both channels, then write when `poll`
says the device takes a block and read when it has one, with nothing written
ahead. An overrun of the module's three-block ring stops the channel for
good (the line helper then exits and is started again).

**LAN:** `eth1` does not exist at boot. It is made on top of the Ethernet
controller's `unm` with `unicfg -gwadd eth1 0 <MAC> 0x2f`, then
`swcfg -sw 0 5 9 0x28` and `swcfg -sw 0 5 0xa 0xb0c8` (the maker's
application's commands, read off a box starting up).

**Lights** (`ffxav_led.ko`, `/dev/led`, argument `{u8 led, a, b, pad; u32
count, period}`): command 1 on, 2 off, 4 blink between states `a` and `b`.
Light 0 is the power light: the module starts it blinking; the maker's
application leaves it steady (blink 0,1 for ever, then off).

**Watchdog:** the launcher loads `ffxav_wdt.ko` as the maker's start script
does and never opens it; the box then runs indefinitely (the maker's
application opens it and feeds it every half second).
