# fw/: the image for the box. Kernel (mainline 6.6 plus our board support in
# kernel/), busybox, our daemon, in one zImage with the device tree appended.
#
#   make            build/v90d.zimage (kernel + initramfs + device tree)
#   make DEBUG=1    the same with a root shell on telnet and /dev/mem tools
#   make DEBUG=1 KARGS='...'   more kernel arguments (bring-up: netconsole, initcall_blacklist)
#   make uimage     build/v90d.uimage: the system image, as U-Boot's bootm and
#                   our loader want it, and what `v90nv upgrade` and the
#                   Firmware page take (pass the same DEBUG and KARGS)
#   make release    build/release/v90d-ht802fw.bin: the one file for the stock
#                   firmware's upgrade page (and for our own Firmware page),
#                   with a system built here without the bring-up shell
#   make release ALLOW_DEBUG=1
#                   build/release/v90d-ht802fw-bench.bin, for the bench: the
#                   same around the build/v90d.uimage that is there
#   make tools      the helpers; make test-v90nv, make test-release: the tests
#
# Needs dl/linux-$(KVER).tar.xz and dl/busybox-*.tar.bz2 unpacked under build/
# (see README.md) and the daemon built (make -C ../srv arm).
KVER   := 6.6.63
TOP    := $(abspath ..)
CROSS  := $(patsubst %gcc,%,$(wildcard $(TOP)/toolchain/*/bin/arm-linux-gcc))
LINUX  := $(CURDIR)/build/linux-$(KVER)
KOUT   := $(CURDIR)/build/k-ht802
KMAKE  := $(MAKE) -s -C $(LINUX) O=$(KOUT) ARCH=arm CROSS_COMPILE=$(CROSS)
LIST   := $(CURDIR)/build/initramfs.list
DTB    := $(KOUT)/arch/arm/boot/dts/dspg/dvf99-ht802.dtb

# one place for the version: the web pages show it, the image and the
# firmware file carry it
VERSION := $(shell cat $(CURDIR)/VERSION)
NV_SRC   := tools/v90nv.c tools/nvflash.c tools/nvimage.c tools/nvboot.c tools/nvhttp.c
NV_FLAGS := -std=gnu99 -Wall -Wextra -Werror -Iloader -Idrv -I../cfg -DV90D_VERSION='"$(VERSION)"'

.PHONY: all kernel uimage tools clean-kernel
all: build/v90d.zimage

NV_DEP := $(NV_SRC) tools/nv.h tools/fwserve.h loader/boot.h drv/qspi.h ../cfg/page.h ../cfg/sbuf.h VERSION
tools/v90nv.host: $(NV_DEP)
	cc $(NV_FLAGS) -O2 -o $@ $(NV_SRC)
tools/v90nv.arm: $(NV_DEP)
	$(CROSS)gcc $(NV_FLAGS) -Os -static -o $@ $(NV_SRC)
	$(CROSS)strip $@

tools: tools/v90nv.host tools/v90nv.arm
	$(CROSS)gcc -Os -static -Wall -o tools/peek.arm tools/peek.c
	$(CROSS)gcc -std=gnu99 -O1 -static -Wall -Wextra -o tools/slicpoke.arm tools/slicpoke.c drv/proslic.c drv/slic.c
	$(CROSS)gcc -std=gnu99 -O1 -static -Wall -Wextra -o tools/tdmpoke.arm tools/tdmpoke.c
	$(CROSS)strip tools/peek.arm tools/slicpoke.arm tools/tdmpoke.arm

# what goes into the image besides the kernel: built first, so that an image
# never carries a flash tool or a daemon older than its sources
.PHONY: parts
parts: tools/v90nv.arm
	$(MAKE) -s -C ../srv v90d-arm
	$(MAKE) -s -C ../arch v90arch-arm

kernel: parts
	./kernel/apply.sh $(LINUX)
	cp kernel/ht802_defconfig $(LINUX)/arch/arm/configs/ht802_defconfig
	./mkinitramfs.sh $(if $(DEBUG),debug) > $(LIST)
	$(KMAKE) ht802_defconfig
	$(LINUX)/scripts/config --file $(KOUT)/.config --set-str INITRAMFS_SOURCE "$(LIST)"
	$(if $(KARGS),$(LINUX)/scripts/config --file $(KOUT)/.config --set-str CMDLINE "$(KARGS)" -d CMDLINE_FROM_BOOTLOADER -e CMDLINE_EXTEND)
	$(KMAKE) olddefconfig
	$(KMAKE) -j$$(nproc) zImage dtbs

build/v90d.zimage: kernel
	cat $(KOUT)/arch/arm/boot/zImage $(DTB) > $@
	@ls -l $@

# legacy uImage, not compressed (the zImage unpacks itself), as the box's
# U-Boot wants it: load and entry 0xC8008000
# the two stages of U-Boot environment (tools/mkenvlist.py, README.md)
UBOOT_BIN := $(TOP)/recon/backup/mtd_uboot.bin
.PHONY: env
env: tools
	@for s in save boot; do python3 tools/mkenvlist.py $(UBOOT_BIN) $$s > build/env-$$s.list && \
	  tools/v90nv.host env-write build/env-$$s.bin build/env-$$s.list && echo build/env-$$s.bin; done

uimage: build/v90d.zimage tools/mkuimage.py
	python3 tools/mkuimage.py build/v90d.zimage build/v90d.uimage $(VERSION)

# The one file an owner uploads on the stock firmware's upgrade page, and
# later on our own Firmware page (DESIGN.md "Release image"; tools/mkgsfw.py;
# loader/).
#   REL_VERSION  what the three parts call themselves: 90. and our version.
#                The stock upgrader writes a part whose version differs from
#                the one it runs; there is no "newer than" rule, and no stock
#                version starts with 90.
#   REL_BITS     hardware bits. The stock value admits every board the stock
#                firmware knows. Once /proc/gxp/dev_info/required_bits of the
#                unit this was proven on is known, put exactly that here and
#                the upgrader of any newer board refuses the file.
# Plain `make release` builds its own system image, without the bring-up
# shell and without extra kernel arguments, into build/release/ (the kernel
# tree is shared, so a bench build after it links the kernel again; the
# bench image build/v90d.uimage itself is not touched). With ALLOW_DEBUG=1
# the image that is in build/ is wrapped as it is, bring-up shell and all,
# under a name that says so.
# The checks after the build: the stock upgrader's own (re-implemented in
# mkgsfw.py verify), the files as they would stand in flash put before
# the loader's deciding part on the host, and the running system's own
# check of a file it is given (v90nv check, on a made-up box).
REL_VERSION ?= 90.$(VERSION)
REL_BITS    ?= 0000:0000:0000:7fff
REL         := build/release
REL_FILE    := $(REL)/v90d-ht802fw$(if $(ALLOW_DEBUG),-bench).bin
REL_UIMAGE  := $(if $(ALLOW_DEBUG),build/v90d.uimage,$(REL)/v90d.uimage)
REL_ZIMAGE  := $(if $(ALLOW_DEBUG),build/v90d.zimage,$(REL)/v90d.zimage)
.PHONY: release release-image loader test-release test-v90nv
loader:
	$(MAKE) -C loader
release-image:
	@mkdir -p $(REL)
	$(MAKE) kernel DEBUG= KARGS= LIST=$(CURDIR)/$(REL)/initramfs.list
	cat $(KOUT)/arch/arm/boot/zImage $(DTB) > $(REL)/v90d.zimage
	python3 tools/mkuimage.py $(REL)/v90d.zimage $(REL)/v90d.uimage $(VERSION)
release: loader tools/v90nv.host $(if $(ALLOW_DEBUG),,release-image)
	@test -f $(REL_UIMAGE) || { echo "$(REL_UIMAGE) is not there: make uimage DEBUG=1 (it builds the kernel)"; exit 1; }
	@if [ -z "$(ALLOW_DEBUG)" ] && grep -q debug.note $(REL)/initramfs.list; then echo "the release image has the bring-up shell in it"; exit 1; fi
	@mkdir -p $(REL)
	@printf 'v90d: this partition is not used. It is here because the stock boot\nloader only starts a set whose third part names the other two.\n' > $(REL)/prog.txt
	python3 tools/mkgsfw.py build -o $(REL_FILE) --version $(REL_VERSION) --bits $(REL_BITS) \
	    --core loader/build/loader.uimage --base $(REL_UIMAGE) --prog $(REL)/prog.txt
	python3 tools/mkgsfw.py verify $(REL_FILE)
	@python3 tools/mkgsfw.py extract $(REL_FILE) $(REL)/flash > /dev/null
	$(MAKE) -s -C loader release-test CORE=$(CURDIR)/$(REL)/flash/ht802core.bin BASE=$(CURDIR)/$(REL)/flash/ht802base.bin ZIMAGE=$(CURDIR)/$(REL_ZIMAGE)
	@python3 tools/test_v90nv_boot.py --make-flash $(REL)/box.bin $(REL)/box.uimage && \
	  tools/v90nv.host -f $(REL)/box.bin check $(REL_FILE) && tools/v90nv.host -f $(REL)/box.bin check $(REL_UIMAGE); \
	  r=$$?; rm -f $(REL)/box.bin $(REL)/box.uimage; exit $$r
	@ls -l $(REL_FILE)

# the running system's flash tool: the plain commands, and what it does to a
# box (where, confirm, to-stock, upgrade, settings, the web page's two
# actions, power cuts), for the host and for the box's processor under qemu
QEMU ?= qemu-arm-static
test-v90nv: tools/v90nv.host tools/v90nv.arm
	tools/test_v90nv.sh tools/v90nv.host
	tools/test_v90nv.sh tools/v90nv.arm $(QEMU)
	@$(MAKE) -s -C loader build/flash_decide
	python3 tools/test_v90nv_boot.py
	V90NV="$(QEMU) $(CURDIR)/tools/v90nv.arm" python3 tools/test_v90nv_boot.py

# everything about the release that can be tested without a box
test-release: test-v90nv
	tools/test_confpack.sh
	tools/test_startgood.sh
	tools/test_bbsfetch.sh
	python3 tools/test_mkgsfw.py
	python3 tools/test_release.py
	$(MAKE) -s -C loader test test-arm
