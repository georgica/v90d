# v90d on the Obihai OBi200

The OBi200 (one telephone port) runs v90d on **our own Linux 6.6 kernel**,
with drivers for this box written for v90d (`fw/kernel`), on a root file
system of our own (see "Our own root file system" below). The box's root
file system and `/obi` parts of the flash are replaced: U-Boot, the maker's
kernel in the flash and the recovery mode stay as they are. At every start
the maker's kernel boots first and `/obi/obi` then starts ours from RAM (see
"Two kernels" below). If ours does not come up, the box stays on the
maker's kernel, where the same v90d runs too.

Each release carries **`v90d-obi200.fw`**, ready to install. It is built by
the release CI from the community firmware file arrynrob13 (fetched and
checked by `getbase.sh`): it is that whole file with our `/obi` in place of
the maker's, the maker's telephony, LED and watchdog kernel modules copied
into it. You can also build it yourself, below.

Verified on a unit (OBi200 hardware 1.4, community firmware arrynrob13, build
8680EX):

- a U.S. Robotics 56K (5.4.62) calls in at **V.92 54 666 / 33 333 bit/s**, LAPM
  with V.42bis, and gets PPP and the Internet through the box's NAT
- the box starts our kernel by itself from the flash, also after the power
  is cut and comes back
- updates through v90d's own Firmware page under either kernel, and back
  again (see below)
- the power light blinks while the box starts and is steady once the line
  is up; the phone light is on while the telephone is off hook

Not tried: the OBi202 (two ports), other OBi models, other base firmware
files.

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
whole firmware file (about 14 MB), identical to the base except for `/obi`.
It builds our kernel too (in `fw/build/k-obi200-rel`, from the Linux sources
`fw/setup.sh` fetched); `make -C fw/obi kernel` builds only that.
`make -C fw/obi test BASE=...` also runs the flash tool's tests.

## Install

On the OBi's own web page (System Management, Device Update), upload
`v90d-obi200.fw` as you would any firmware. The box writes it and restarts
into v90d, about three minutes. It then answers at the address it gets by
DHCP, with v90d's pages (user `admin`, password `admin` until you change it
on the Settings page). The telephone port is v90d's line; the settings live in
the box's writable flash (`/scratch/v90d`) and survive updates.

There is no SSH, as on the HT80x: the status and settings pages are the way
in, and the rescue address 169.254.90.1 when the LAN has none. (Our root
file system starts no dropbear or telnetd under either kernel.)

## Update

On v90d's **Firmware** page, upload a newer `v90d-obi200.fw` (from a
release, or built the same way). The page checks the whole file, writes only what differs from the flash
(normally `/obi` and the root file system), and restarts the box.

## Going back

Upload the community firmware file itself (BASE) on v90d's Firmware page. It
differs from the flash in `/obi` and the root file system, so those are
written, and the box restarts into the maker's application with its old
settings.

If the power goes while the flash is written, or something else leaves the box
unable to start, it comes up in the maker's **recovery mode**: a plain page
titled "OBi202 Firmware Update" at the box's address, which takes any firmware
file (BASE, or v90d's). v90d never writes the boot loader or the recovery
images, so that mode is always there.

## Two kernels

U-Boot only checks the flash's checksums before it boots a kernel and has no
fallback, so our kernel is not written to the maker's kernel slot: a kernel
that stopped half way would leave a box that only a serial cable could save.
Instead:

1. U-Boot boots the maker's kernel; its `/etc/rc` runs `/obi/obi`.
2. `/obi/obi` loads the maker's modules, then starts `/obi/kernel.xz` from
   RAM with `kjump` (fw/obi/ramboot), unless it did not come up twice in a
   row since the power came on (a count in RAM, `kjump --count`) or the file
   `/scratch/v90d/maker-kernel` exists. `kjump` unpacks the kernel while the
   maker's kernel still runs (and feeds the watchdog), then jumps.
3. Our kernel's whole initramfs is `oinit`: it mounts the box's own root file
   system (its BusyBox and libraries) and `/obi`, and runs `/obi/kinit` as
   process 1, which does what this box needs of the maker's `/etc/rc` (no
   ssh) and runs `/obi/obi` again.
4. `/obi/obi` sees the newer kernel and runs the same programs: the LAN port
   is `eth0`, the switch is set up by the kernel, and the Firmware page writes
   through a flash partition that starts after U-Boot and the factory block.
5. A minute after the daemon runs, `kinit` resets the count.

`/obi` holds the start (`obi`, `kinit`, `kjump`), our kernel, the default
settings and the lists. The programs, `v90d`, `obiline` and `obinv`, are in
`/usr/sbin` of the root file system, which both kernels start from: `/obi`
(2.25 MB) keeps its room for the kernel, which IPv6 made 112 KB bigger.
`obinv` locks itself in memory before it writes, wherever it runs from.

A start takes about 70 s from power on to a ready line. The kernel drivers
(Ethernet and switch, SPI flash, watchdog, LEDs, GPIO, the SLIC and the audio
DMA, which ours takes over from the maker's kernel as it runs) were
reconstructed from the maker's kernel and modules; `fw/kernel/*-NOTES.md`
say what is verified and what is inferred.

## Our own root file system

`v90d-obi200.fw` carries v90d's own root file system (`mkrootfs.py`) in
place of the maker's, in its 6.75 MiB part at 0x480000, so the box has what
the HT802 has: the archive gateway, OpenVPN (the IWANGO games' VPN, its
profile kept in `/scratch/v90d`), the daily update check and the Install
button (`fwupdate.sh`, through obinv), the Dreamcast console's own address
(`dclease.script`), and the clock from NTP. It holds our static BusyBox, our
start files (`rootfs/`: `inittab`, `rcS` for the maker's kernel), the archive
gateway and OpenVPN, and, taken out of BASE when it is built as the modules
are, the maker's `swcfg`, `unicfg` and `iptables` with their libraries (used
only under the maker's kernel). About a third of the part is used. Our kernel
mounts it as before (oinit); the maker's kernel boots it from the flash and
runs `rcS`, which makes `/dev` from `/sys` (this BusyBox's `mdev -s` names
nodes by number on 2.6.30) and starts `/obi/obi`.

Two nets against a box that starts but cannot be reached: `make test` runs
`test_scripts.py`, which checks every command the start scripts run against
the programs of the image, and runs the ones whose options differ between
BusyBox versions with the scripts' own options under qemu-arm; and the box
always answers on the rescue address 169.254.90.1 (as the HT802 does), also
when DHCP gives it nothing: the daemon listens on the LAN port itself, not
only once it has an address.

It runs on a unit (a dial-up call with PPP and NAT through it). Every start goes through this file system, so a
mistake in it is not caught by the boot's checksum test: the box starts, but
may be unreachable. Keep a 3.3 V serial adapter at hand when trying a change.

### Getting back in with a serial cable and a network cable

The serial console (115200 8N1) stops U-Boot when a key comes during its
one-second countdown. Its `sf` flash commands loop forever here: do not use
them. The maker's kernel is already in RAM at 0x800000, and that kernel has a
RAM flash device of 8 MiB at 0x3800000 (its `/dev/mtdblock0`, which its
recovery mode boots from). With the box's LAN port cabled to a computer at
192.168.15.102 that serves TFTP:

    setenv ipaddr 192.168.15.33
    tftpboot 3800000 rescue.sqsh        (a squashfs: BusyBox, obinv, /init, /dev/console)
    tftpboot 4000000 v90d-obi200.fw     (above the kernel's 56 MiB: left alone)
    setenv bootargs console=ttyS0,115200 root=/dev/mtdblock0 rootfstype=squashfs mem=56M init=/init FWSIZE=<octets> FWMD5=<md5>
    bootm 800000

The rescue's `/init` copies the file out of physical memory (through an
mmap of `/dev/mem`: a read() above the kernel's memory is refused), checks
its MD5, and writes it with `obinv serve-upgrade` on the whole chip
(`/dev/mtd6` under the maker's kernel), which restarts the box. `KEY=VALUE`
words of the command line reach init's environment; other words after
`init=` do not reach the program.

## How it works

| piece | what it is |
| --- | --- |
| `obi` | the launcher, run by the system's `/etc/rc` at boot (and by `kinit` under our kernel): loads the maker's modules and starts our kernel, or under ours makes the LAN port, starts the line and the daemon, restarts the box when the Firmware page or the Restart button asks |
| `kernel.xz` | our kernel (fw/kernel, `make -C fw/obi kernel`): the Image with `oinit` as its initramfs and the device tree, packed for `kjump` |
| `kjump` | starts `kernel.xz` from RAM under the maker's kernel (fw/obi/ramboot/kjump.c); `kjump --count` is the start count (bootcount.c) |
| `oinit.c`, `kinit` | our kernel's first steps: the root file system, then the part of the maker's `/etc/rc` the box needs |
| `obiline.c` | the telephone line in user space: SLIC bring-up (fw/drv/slic.c, patch-free), the 10 ms audio loop, the hook, the lights; serves the daemon on `/var/run/v90line` (`seq:` in srv/line.h) |
| `obinv.c` | the Firmware page's flash tool on this box (the HT80x has fw/tools/v90nv) |
| `compat26.c` | `rename` and `accept4` for Linux 2.6.30, older than our C library expects |
| `mkobifw.py` | the firmware file builder; the file format is in its header |
| `udhcpc.script` | the LAN address by DHCP (fw/udhcpc.script, the HT80x's) |

The daemon itself is the same as on the HT80x (`make -C srv v90d-obi`), built
with `compat26.c` and a 16 MiB Firmware page upload. Under our kernel it uses
TUN and nftables as on the HT80x. Under the maker's kernel, which lacks both,
it falls back by itself: the PPP link's network device is the kernel's own
PPP unit over a pseudo-terminal (srv/tun.c), and NAT goes through the
system's `iptables` (srv/nat.c).

## Facts about the box

Read off the unit (the maker's binaries were studied, not used in ours).

**Flash** (16 MiB SPI, `/dev/mtd6` is all of it, 64 KiB sectors): U-Boot
0x0, factory block "OB100 UNIT INFO" 0x40000 (the unit's MAC at 0x40100),
kernel 0x50000, recovery kernel 0x2D0000, **header of the last firmware file
written 0x470000** (the boot checks the partitions against it: a firmware
file that does not carry every part leaves the box in recovery mode), root
file system 0x480000 (0x6C0000), `/scratch` (jffs2) 0xB40000 (0xC0000), `/obi` 0xC00000 (0x240000),
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
