Vendor source for exactly this board is public: Grandstream's HT8xx GPL tarball is still live and contains the 3.4.20-rt31 DVF kernel and the `evb9918_ht` U-Boot. There is no mainline, OpenWrt or mainline U-Boot support.

I downloaded and unpacked the tarball, so the claims about its contents are from inspection; nothing was built.

## 1. Mainline / OpenWrt / U-Boot support: none found

- **Linux mainline:** no `arch/arm/mach-dvf`, `mach-dspg` or `mach-dmw` in torvalds/linux master, and no `dvf` entry in `arch/arm/tools/mach-types` at v3.4, v4.19 or v6.6. No `dspg` vendor prefix turned up in `vendor-prefixes.yaml`. High confidence. https://github.com/torvalds/linux/tree/master/arch/arm
- **Mainline U-Boot:** no `arch/arm/mach-dspg`. Only the machine IDs exist in `arch/arm/include/asm/mach-types.h`: `MACH_TYPE_DVF99 4279` and `MACH_TYPE_DMW96 2866`. High confidence. https://github.com/u-boot/u-boot/blob/master/arch/arm/include/asm/mach-types.h
- **OpenWrt:** no DSPG target in `target/linux/`. Forum searches returned nothing relevant. High confidence for the tree, medium for the forum. https://github.com/openwrt/openwrt/tree/main/target/linux
- **LKML/lore:** not verified. lore.kernel.org blocked the query with a bot check, and web searches found no DVF99 patch submissions.
- **GitHub:** code search for `gpio-dspg`, `dspg-qspic`, `spi-dspg-dcx`, `dvf9a`, `evb9918`, `mach-dvf` and `dspg,tdm` found only boot logs, no source mirror. `dspg,dvf99` and `dvf99.dtsi` also returned nothing.

## 2. Vendor source

**Grandstream HT8xx GPL tarball (the main find)**
- Live URL: https://www.grandstream.com/hubfs/gnu_gpl/ht8xx_gpl.tar.gz (395,068,538 bytes, sha256 `b14ae733ab897cddba2ddc34d7e0e0372a499b06f85e260f2abe148a927cc8e7`).
- Byte-identical Wayback copy from 2017-09-08: http://web.archive.org/web/20170908011128/http://www.grandstream.com/sites/default/files/Resources/ht8xx_gpl.tar.gz
- Current index page: https://www.grandstream.com/legal/open-source-software (the old `/support/faq/gnu-general-public-license` URL now redirects to a page without the links).
- The archive is dated 2017-04-28 and holds `linux-3.4.20.tgz`, `uboot.tgz`, busybox, uclibc, dropbear and others under `data/work/SW_ENGR/dvf9/system/opensource/opensource/`.

**Kernel in that tarball: 3.4.20 with `localversion-rt` = `-rt31`**
- **Platform and clock:** `arch/arm/mach-dvf/` includes `clock.c` (2001 lines), `ccu.c`, `time.c`, `pm.c`. `arch/arm/mach-dmw/` is also present.
- **UART:** `drivers/tty/serial/dspg-uart.c`.
- **QSPI flash:** `drivers/mtd/devices/dspg-qspic.c`.
- **GPIO:** `drivers/gpio/gpio-dspg.c`.
- **SPI:** `drivers/spi/spi-dspg-dcx.c`.
- **TDM/PCM:** `sound/soc/dvf/dspg-tdm.c` (1583 lines, "DSPG ASoC CPU DAI driver"), `snd-dvf99-pcm.c`, `snd-dvf99-cpu-dai.c`, `css-codec.c`, `dvf-evb-si32178.c`, plus `include/sound/dvf-tdm.h`.
- **CSS/coma:** `drivers/staging/dspg/css.c` and `drivers/staging/dspg/coma/` (including `coma-tdm.c`, `coma-voice.c`).
- **Other:** `drivers/mfd/dvf9a-core.c`, `drivers/watchdog/dspg_wdt.c`, `drivers/clocksource/clksrc-dspg-timer.c`.
- **Device trees:** `arch/arm/boot/dts/dvf99.dtsi`, `dvf9918-evb.dts`, and a `devtree/` directory with per-board files: `ht802_9610003703a.dts` through `ht802_9610003713b.dts`, plus ht812, ht814 and `gxp1625.dts`.
- **Configs and build:** `defconfig_ht80x`, `defconfig_ht80x_nfs`, `defconfig_ht81x`, `defconfig_gxp`. `build.sh` expects the toolchain at `/opt/dspg/v1.2.6.1-rc2/` (`arm-dspg-linux-uclibceabi-`).
- **TDM ownership:** `dvf99.dtsi` has `tdm@05f00000` and `tdm@06000000` (`compatible = "dspg,tdm"`). The HT802 dts enables TDM0 and sets `css { tdm-ownership = <0x00000003>; }`, i.e. the stock config hands TDM to the CSS core.
- **Build gap:** `include/provision.h` is a dangling symlink to `../../../../provision/provision.h`, which is not in the tarball. It is referenced by `arch/arm/mach-dvf/provision.c`, `factory.c` and `drivers/nvram/nvram_linux.c`. Whether the tree builds without it is untested.

**U-Boot in that tarball: 2012.04.01-based**
- `boards.cfg` has `evb9918_ht` (HT80x, text base 0xC8400000) and `evb99_ht_qspi` (HT81x). Board code is in `board/dspg/evb9918/`.
- `build.sh` calls `../bootastic/mkbootimg.ht80x` to make a `u-boot.xbin` "for UART booting". The Bootastic source and that tool are not in the tarball.

**Other sources**
- **Astralix/hardware_drivers** has DSPG DMW96 U-Boot plus Bootastic source (`bootastic.c`, `mkbootimg.c`, `cmu.c`, `ddr.c`). It is a sister SoC, not DVF99; applicability is unverified. https://github.com/Astralix/hardware_drivers/tree/master/dmw96/u-boot/bootastic
- **Other Grandstream tarballs** are live in the same directory but I did not download them: `gxp16xx_opensource.tar.bz2_.zip` (123 MB), `GXP17xx_GPL.tar.gz` (325 MB), `dp7xx_gpl.zip` (380 MB), `GXW42xx_GPL.zip` (133 MB), all under `https://www.grandstream.com/hubfs/gnu_gpl/`. GXP16xx is very likely the same SoC, since the HT8xx kernel carries `gxp1625.dts`.
- **Yealink "Linux Kernel.zip"** is actually a RAR of linux-2.6.10 with no dspg/dvf strings; not relevant. https://www.yealink.com/en/onepage/gpl-open-source-downloads
- **Not found or not verified:** DVF99 kernel tarballs from Gigaset, Snom, Panasonic (KX-TGP600, KX-NT550), Obihai (OBi1032/1062), or Wildix. Press releases confirm those vendors use DVF99, but I located no source.

## 3. Datasheet / register manual: none public

- Only a marketing family brief (original link now redirects and returns 403) and broker listings for DVF99185AE2BBC turned up. https://www.symmetryelectronics.com/getmedia/f83e6efe-7b0e-4973-aa57-b2e89dce7f28/enterprise_voice_dvf_family-d819b.pdf
- The practical register reference is the GPL source: `mach-dvf/clock.c`, `mach-dvf/include/mach/hardware.h`, `gpio-dspg.c`, `dspg-tdm.c`, and U-Boot's `board/dspg/evb9918/tdm.c` and `arch/arm/cpu/arm926ejs/dvf99/`.

## 4. Bootloader and prior art

**From the GPL U-Boot source (high confidence for that source, 2017 vintage)**
- **Autoboot:** `CONFIG_BOOTDELAY 1`, `CONFIG_BOOTABORTKEY 0x1b` (Esc only), prompt `DVF99 # `, 115200 baud. There is no password code in `abortboot()`.
- **Boot command:** `bootcmd=gsboot` (`common/cmd_gsboot.c`). It checks only the Grandstream magic 0x23C97AF9, a 16-bit checksum and version compatibility; there is no RSA/SHA verification.
- **Secure boot:** `CONFIG_DVF99_SECURE_BOOT` is commented out in `include/configs/dvf99.h`, though the code exists in `board/dspg/evb9918/secure_boot/`.
- **TFTP:** the env has `tftp_boot=load_cfg;tftp ${loadaddr} core.${GSMODEL}.nfs; tftp ${devtree_addr} ${MODEL}_${PNSTR}.dtb; run bootm`. Defaults are ipaddr 192.168.0.240, serverip 192.168.0.1, load address 0xC8800000, devtree 0xC8C00000.
- **Flashing:** `updatecore`, `updateboot` and similar env commands do tftp plus `qspi write.partial`.

**Shipped bootloaders differ from the GPL source**
- The 1.0.9.3 `ht802boot.plain` in your local FirmwareDumps (built Feb 01 2018) still has "Hit Esc key to stop autoboot" and `tftp_boot`. It adds `console=no` in the default env and a `secure` string that the GPL tree lacks.
- I found no password strings in it, but did not disassemble it. Whether Esc still gives a prompt on current firmware is unverified.

**Field reports**
- **Serial console:** 3.3 V TTL at 115200. The Linux root shell on serial worked on early firmware and was used for the Vonage unlock (toro, Dec 2016). "Anything newer than 1.0.3.2 has the serial console shell disabled" (jsolo1, 2017-10-19); downgrading restored it. That concerns the Linux shell; nobody in the pages I could read mentions a U-Boot password. Medium confidence, and only part of the thread was retrievable. http://web.archive.org/web/20171021152951/http://www.dslreports.com:80/forum/r30988922-Unlock-New-Grandstream-ATA-used-by-Vonage-Unlocked~start=330
- **Firmware containers:** AES-encrypted with a static key and a checksum, not signed. BigNerd95's GSFW.py repacks modified images, but only a squashfs (prog) repack is documented, not a kernel replacement. Issues are disabled on that repo. https://github.com/BigNerd95/Grandstream-Firmware-HT802
- **Root without custom firmware:** the `gssu` challenge/response over SSH (same repo, `SuperUser/`).
- **Another DVF99 device with an open prompt:** a Wildix-SDK phone running U-Boot 2015.01 "evb99-qspi" reaches `DVF99 #` via Esc and exposes `tftpboot`, `loady`, `otpdump` and `securedev` (burns OTP to enforce secure boot). So the SoC has OTP-enforced secure boot available. Whether Grandstream burned it on the HT802 is unverified; the GPL config suggests not. https://github.com/CaptureTheFrog/ip-phones/blob/main/HPm/uart.txt
- **HT802 boot log** (bootrom v2.0, Bootastic, U-Boot evb9918-ht, mtdparts): https://github.com/danry25/HT802/blob/master/ATA-1/bootlog.md
- **HT812/HT814 teardowns** (DVF99195 SoC, same Bootastic/U-Boot chain; no prompt details): http://www.natthapol89.site/2019/09/grandstream-ht812.html and http://www.natthapol89.site/2022/09/grandstream-ht814-teardown.html
- **Custom kernels:** I found no report of anyone running a self-built kernel on HT801/802/812/814 or GXP16xx.

## Local files

Everything is in the session scratchpad, which is not durable; copy what you want to keep.
- Tarball: `/tmp/v90d-scratch/gs_live/ht8xx_gpl.tar.gz`
- Kernel tree: `/tmp/v90d-scratch/x/k/linux-3.4.20`
- U-Boot tree: `/tmp/v90d-scratch/x/u/uboot`