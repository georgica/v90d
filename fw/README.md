# fw/ - the firmware image around the daemon (Track A)

Runs on the HT802 from flash since 2026-11-15 (bring-up build; evidence in
PROGRESS.md, hardware facts in HARDWARE.md, boot scheme in DESIGN.md).

- `Makefile`: `make` builds `build/v90d.zimage` (kernel, device tree,
  initramfs); `make DEBUG=1 [KARGS='...']` adds a telnet root shell and
  /dev/mem tools; `make uimage` wraps it for U-Boot (pass the same DEBUG and
  KARGS, or it rebuilds the plain image); `make env` builds the U-Boot
  environments; `make tools` the helpers.
- `kernel/`: our board support, applied to Linux 6.6.63 by `kernel/apply.sh`
  (interrupt controller, timer, serial, GPIO, clock and reset unit, watchdog,
  flash, line driver `v90line`).
- `drv/`: the portable parts of the drivers (SLIC, pump, flash), with host
  tests: `make -C drv test test-arm`.
- `rcS`, `udhcpc.script`, `v90d.conf`: boot script, DHCP lease handler,
  factory settings. `rcS` asks `v90nv where` how the box was started, keeps
  the settings where that scheme has them and confirms a good start after a
  minute.
- `VERSION`: the version, one line; it goes into the image name, the
  firmware file and the pages.
- `tools/ramboot.sh`: start an image from RAM out of the stock firmware.
  `tools/v90nv` (`v90nv.c`, `nvboot.c`, `nvflash.c`, `nvimage.c`,
  `nvhttp.c`): everything that reads or writes the flash from the running
  system. `tools/mkuimage.py`, `tools/mkenvlist.py`: image header,
  environment list.

`v90nv` on the box (it finds the flash and the scheme itself; `-f FILE`
points it at a 16 MiB image of the flash instead, which is how the tests
run it):
- `v90nv where`: scheme (`loader`, `old`, `none`), set, counts, what the
  next start will run, whether the stock set is good.
- `v90nv confirm`: this start was good.
- `v90nv check FILE`, `v90nv upgrade FILE`: FILE is `build/v90d.uimage` or a
  firmware file from `make release`; checked completely before the first
  write, signature written last. Then reboot.
- `v90nv to-stock`: the next start is the stock firmware. On the bench
  scheme this removes `v90d_tries`; from the stock root shell
  `v90nv env-set /dev/mtd13 v90d_tries 3` brings ours back.
- `v90nv conf-save FILE`, `v90nv conf-load FILE`: the settings.
The same two actions are on the "Firmware" page of the web interface. The
older commands that name an MTD device (`env-get`, `env-set`,
`blob-load`, `blob-save`) are still there for the stock shell.

Programs the image needs besides the kernel: busybox (sh, mount, ifconfig,
route, udhcpc, ntpd, syslogd optional) and `srv/build/v90d-arm` (static,
about 360 kB). NAT needs no user-space tool: the daemon installs its one
nftables rule over netlink.

Release image for the stock upgrade page (design in DESIGN.md, not yet run
on the box): `loader/` is the small first stage that goes into `core`
(`make -C loader test test-arm`), `tools/mkgsfw.py` reads, checks and
builds stock-format firmware files (`list`, `verify`, `build`, `extract`),
`make release` builds its own image without the bring-up shell and from it
`build/release/v90d-ht802fw.bin`; `make release ALLOW_DEBUG=1` wraps the
bench image in `build/` as `v90d-ht802fw-bench.bin`; `make test-release`
runs every test around it (`make test-v90nv` those of `v90nv` alone).
