# fw/obi/ramboot: boot a kernel from RAM on the OBi200 running its stock
# kernel (which has no kexec), for bring-up without a serial console.
#   make                    kjump (for the box), stub.bin, shim.bin
#   make payload ZIMAGE=... DTB=...   build/payload.bin: shim, zImage, dtb
# On the box: ramboot.sh (stops the telephony and the network, whose DMA
# would write into the new kernel's memory, then kjump).
ARM    ?= $(patsubst %gcc,%,$(wildcard $(CURDIR)/../../../toolchain/*/bin/arm-linux-gcc))
BUILD  := build
.PHONY: all payload image clean
all: $(BUILD)/kjump $(BUILD)/shim_image.bin $(BUILD)/tdmoff $(BUILD)/physput $(BUILD)/physdump $(BUILD)/stub.bin $(BUILD)/shim.bin
$(BUILD):
	@mkdir -p $(BUILD)
# kjump carries the Linux kernel's XZ decoder (lib/xz), built in user space
KSRC   ?= $(CURDIR)/../../build/linux-6.6.63
XZ_SRC := $(addprefix $(KSRC)/lib/xz/,xz_crc32.c xz_dec_stream.c xz_dec_lzma2.c xz_dec_bcj.c)
$(BUILD)/xz.h: $(KSRC)/include/linux/xz.h | $(BUILD)
	cp $< $@
$(BUILD)/kjump: kjump.c ../bootcount.c xz/xz_config.h $(BUILD)/xz.h | $(BUILD)
	$(ARM)gcc -static -O2 -std=gnu99 -Wall -Wextra -I$(BUILD) -Ixz -o $@ kjump.c ../bootcount.c $(XZ_SRC)
	$(ARM)strip $@
$(BUILD)/%: %.c | $(BUILD)
	$(ARM)gcc -static -O2 -std=gnu99 -Wall -Wextra -o $@ $<
	$(ARM)strip $@
$(BUILD)/%.bin: %.S | $(BUILD)
	$(ARM)gcc -c -o $(BUILD)/$*.o $<
	$(ARM)objcopy -O binary $(BUILD)/$*.o $@
payload: $(BUILD)/shim.bin
	@test -n "$(ZIMAGE)" -a -n "$(DTB)" || { echo "make payload ZIMAGE=... DTB=..."; exit 1; }
	cat $(BUILD)/shim.bin $(ZIMAGE) $(DTB) > $(BUILD)/payload.bin
	@ls -l $(BUILD)/payload.bin
# the kernel unpacked, for kjump to unpack: make image IMAGE=... DTB=...
image: $(BUILD)/shim_image.bin
	@test -n "$(IMAGE)" -a -n "$(DTB)" || { echo "make image IMAGE=... DTB=..."; exit 1; }
	./mkpayload.py $(BUILD)/shim_image.bin $(IMAGE) $(DTB) $(BUILD)/payload.xz
clean:
	rm -rf $(BUILD)
