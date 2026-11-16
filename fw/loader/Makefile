# fw/loader: the small program of ours that the stock boot loader starts from
# the "core" partition, and that finds our system image in "base", checks it
# and starts it (boot.h; DESIGN.md "Release image").
#
#   make test        the decisions, on the host, against the flash model of fw/drv/test
#   make test-arm    the same, built for the box and run under qemu
#   make             build/loader.uimage for the box (linked at LD_LINK): the payload of "core"
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
CROSS   := $(patsubst %gcc,%,$(wildcard $(CURDIR)/../../toolchain/*/bin/arm-linux-gcc))
QEMU    ?= qemu-arm-static
BUILD   := build
LD_LINK := 0xC9000000
DRV     := ../drv
SRC     := boot.c $(DRV)/qspi.c
HDR     := boot.h $(DRV)/qspi.h
# no library, no startup files, nothing the compiler might want to call
BARE    := -std=c99 -Os -Wall -Wextra -Werror -ffreestanding -fno-builtin -nostdlib -nostartfiles \
           -marm -mcpu=arm926ej-s -msoft-float -fno-pic -fno-stack-protector -ffunction-sections -fdata-sections

.PHONY: all test test-arm release-test clean
all: $(BUILD)/loader.uimage
$(BUILD):
	@mkdir -p $(BUILD)
$(BUILD)/test_boot: test/test_boot.c $(SRC) $(HDR) $(DRV)/test/qspisim.c | $(BUILD)
	$(CC) $(CFLAGS) -I../../test -I$(DRV) -I$(DRV)/test -o $@ test/test_boot.c $(SRC) $(DRV)/test/qspisim.c
$(BUILD)/arm_test_boot: test/test_boot.c $(SRC) $(HDR) $(DRV)/test/qspisim.c | $(BUILD)
	$(CROSS)gcc $(CFLAGS) -static -I../../test -I$(DRV) -I$(DRV)/test -o $@ test/test_boot.c $(SRC) $(DRV)/test/qspisim.c
test: $(BUILD)/test_boot
	@./$(BUILD)/test_boot
test-arm: $(BUILD)/arm_test_boot
	@$(QEMU) ./$(BUILD)/arm_test_boot

$(BUILD)/loader.elf: start.S hw.c $(SRC) $(HDR) loader.lds | $(BUILD)
	$(CROSS)gcc $(BARE) $(LD_EXTRA) -I$(DRV) -Wl,-T,loader.lds -Wl,--defsym=LD_LINK=$(LD_LINK) -Wl,--gc-sections -Wl,--build-id=none -Wl,--no-warn-rwx-segments -static \
	    -o $@ start.S hw.c $(SRC) -lgcc
$(BUILD)/loader.bin: $(BUILD)/loader.elf
	$(CROSS)objcopy -O binary $< $@
	@# whole cache lines: the stock boot loader copies the image to its load
	@# address and then cleans the data cache over four times its length, but
	@# gives that up without a word unless the range ends on a 32-octet
	@# boundary (HARDWARE.md, "How gsboot starts what is in core")
	@truncate -s %32 $@
	@$(CROSS)nm $< | grep -q ' [Uu] ' && { echo "loader: undefined symbols"; exit 1; } || true
	@ls -l $@
# as `bootm` wants it: a U-Boot image header that names LD_LINK as the place
# to put it and to start it. This is the payload of "core"; the stock
# partition header goes in front when the firmware file is made
# (tools/mkgsfw.py, `make -C .. release`).
$(BUILD)/loader.uimage: $(BUILD)/loader.bin ../tools/mkgsfw.py
	python3 ../tools/mkgsfw.py uimage -o $@ --name v90d-loader --load $(LD_LINK) $<

# the files of a release, as they would stand in flash, through the loader's
# deciding part: make release-test CORE=... BASE=... ZIMAGE=...
$(BUILD)/test_release: test/test_release.c $(SRC) $(HDR) $(DRV)/test/qspisim.c | $(BUILD)
	$(CC) $(CFLAGS) -I../../test -I$(DRV) -I$(DRV)/test -o $@ test/test_release.c $(SRC) $(DRV)/test/qspisim.c
release-test: $(BUILD)/test_release
	@./$(BUILD)/test_release $(CORE) $(BASE) $(ZIMAGE)
# the loader's decision on a whole flash image in a file (for fw/tools/test_v90nv_boot.py)
$(BUILD)/flash_decide: test/flash_decide.c $(SRC) $(HDR) $(DRV)/test/qspisim.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(DRV) -I$(DRV)/test -o $@ test/flash_decide.c $(SRC) $(DRV)/test/qspisim.c
clean:
	rm -rf $(BUILD)
