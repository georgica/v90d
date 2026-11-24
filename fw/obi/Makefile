# fw/obi: v90d on the Obihai OBi200 (the maker's kernel and telephony module,
# our programs in place of its application). See README.md.
#   make                  the line helper
#   make image BASE=F.fw  build/v90d-obi200.fw from the owner's firmware file F
ARM_CC  ?= $(wildcard $(CURDIR)/../../toolchain/*/bin/arm-linux-gcc)
CFLAGS  ?= -std=gnu99 -O2 -Wall -Wextra -Werror
DRV     := ../drv
BUILD   := build
VERSION := $(shell cat ../VERSION)
.PHONY: files image-rootfs all image test clean kernel
all: $(BUILD)/obiline $(BUILD)/obinv $(BUILD)/kernel.xz
$(BUILD):
	@mkdir -p $(BUILD)
$(BUILD)/obiline: obiline.c compat26.c $(DRV)/pump.c $(DRV)/proslic.c $(DRV)/slic.c $(wildcard $(DRV)/*.h) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -pthread -I$(DRV) -o $@ obiline.c compat26.c $(DRV)/pump.c $(DRV)/proslic.c $(DRV)/slic.c
	$(dir $(ARM_CC))arm-linux-strip $@
$(BUILD)/obinv: obinv.c compat26.c ../../ppp/ppp_md5.c ../../cfg/page.h ../../cfg/sbuf.h | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -I../../ppp -I../../cfg -o $@ obinv.c compat26.c ../../ppp/ppp_md5.c
	$(dir $(ARM_CC))arm-linux-strip $@
$(BUILD)/oinit: $(BUILD)/%: %.c | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -Os -o $@ $<
	$(dir $(ARM_CC))arm-linux-strip $@

# v90d's own kernel (fw/kernel), started from RAM by kjump (ramboot/):
# its Image with oinit as the whole initramfs and the board's device tree
# behind the RAM boot shim, packed as .xz (kjump unpacks it).
KSRC    := ../build/linux-6.6.63
KOUT    := $(CURDIR)/../build/k-obi200-rel
KMAKE    = $(MAKE) -s -C $(KSRC) O=$(KOUT) ARCH=arm CROSS_COMPILE=$(patsubst %gcc,%,$(ARM_CC))
kernel: $(BUILD)/kernel.xz
$(BUILD)/kernel.xz: $(BUILD)/oinit FORCE | $(BUILD)
	cd .. && ./kernel/apply.sh build/linux-6.6.63 >/dev/null
	cp ../kernel/obi200_defconfig $(KSRC)/arch/arm/configs/obi200_defconfig
	printf 'dir /dev 755 0 0\nnod /dev/console 600 0 0 c 5 1\ndir /root 755 0 0\nfile /init %s 755 0 0\n' $(CURDIR)/$(BUILD)/oinit > $(BUILD)/initramfs.list
	@mkdir -p $(KOUT)
	$(KMAKE) obi200_defconfig
	$(KSRC)/scripts/config --file $(KOUT)/.config --set-str INITRAMFS_SOURCE $(CURDIR)/$(BUILD)/initramfs.list
	$(KMAKE) olddefconfig
	$(KMAKE) -j$$(nproc) Image dtbs
	$(MAKE) -C ramboot build/kjump build/stub.bin build/shim_image.bin
	./ramboot/mkpayload.py ramboot/build/shim_image.bin $(KOUT)/arch/arm/boot/Image \
	    $(KOUT)/arch/arm/boot/dts/obihai/ffxav-obi200.dtb $@

../../srv/build/v90d-obi: FORCE
	$(MAKE) -C ../../srv v90d-obi
FORCE:
# Our files of /obi, as they go into the image
files: $(BUILD)/obiline $(BUILD)/obinv $(BUILD)/kernel.xz ../../srv/build/v90d-obi obi kinit udhcpc.script ../v90d.conf ../VERSION ../bbsfetch.sh ../../lists/bbs.txt ../../lists/services.txt
	@rm -rf $(BUILD)/image && mkdir -p $(BUILD)/image
	cp obi kinit udhcpc.script $(BUILD)/obiline $(BUILD)/obinv $(BUILD)/kernel.xz ../v90d.conf $(BUILD)/image/
	cp ramboot/build/kjump $(BUILD)/image/kjump
	cp ramboot/build/stub.bin $(BUILD)/image/kjump.stub
	cp ../../srv/build/v90d-obi $(BUILD)/image/v90d
	cp ../VERSION $(BUILD)/image/VERSION
	cp ../bbsfetch.sh $(BUILD)/image/bbsfetch.sh
	cp ../../lists/bbs.txt $(BUILD)/image/bbs.txt
	cp ../../lists/services.txt $(BUILD)/image/services.txt
# v90d's own root file system (mkrootfs.py), in place of the maker's: the
# archive gateway, OpenVPN and the scripts the HT802 has in its /etc
BUSYBOX = ../build/busybox-1.36.1
ROOTFS_ADD = ../../arch/build/v90arch-arm:/usr/sbin/v90arch ../build/vpn/openvpn:/usr/sbin/openvpn \
             ../updatecheck.sh:/etc/updatecheck.sh fwupdate.sh:/etc/fwupdate.sh ../dclease.script:/etc/dclease.script
$(BUILD)/rootfs.sqsh: mkrootfs.py rootfs/etc/inittab rootfs/etc/init.d/rcS rootfs/etc/passwd rootfs/etc/group \
                      fwupdate.sh ../updatecheck.sh ../dclease.script \
                      $(BUSYBOX)/busybox ../../arch/build/v90arch-arm ../build/vpn/openvpn FORCE
	@test -n "$(BASE)" || { echo "make image BASE=your-OBi-firmware.fw"; exit 1; }
	$(MAKE) -s -C $(BUSYBOX) busybox.links
	python3 mkrootfs.py "$(BASE)" $@ --busybox $(BUSYBOX)/busybox $(addprefix --add ,$(ROOTFS_ADD))
../../arch/build/v90arch-arm:
	$(MAKE) -C ../../arch v90arch-arm
# The image: BASE's file with our /obi and our root file system in its place
image: files $(BUILD)/rootfs.sqsh mkobifw.py
	@test -n "$(BASE)" || { echo "make image BASE=your-OBi-firmware.fw"; exit 1; }
	python3 mkobifw.py --full --rootfs $(BUILD)/rootfs.sqsh --files $(BUILD)/image "$(BASE)" $(BUILD)/v90d-obi200.fw
image-rootfs: image
# obinv and the builder on the host, against a stand-in flash (needs BASE and the image)
$(BUILD)/obinv.host: obinv.c ../../ppp/ppp_md5.c ../../cfg/page.h ../../cfg/sbuf.h | $(BUILD)
	cc -std=gnu99 -O2 -Wall -Wextra -Werror -I../../ppp -I../../cfg -o $@ obinv.c ../../ppp/ppp_md5.c
test: $(BUILD)/obinv.host image
	python3 test_obinv.py $(BUILD)/obinv.host "$(BASE)" $(BUILD)/v90d-obi200.fw
	python3 test_scripts.py $(BUILD)/rootfs.sqsh $(BUSYBOX)/busybox
clean:
	rm -rf $(BUILD)
