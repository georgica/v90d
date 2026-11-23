# fw/obi: v90d on the Obihai OBi200 (the maker's kernel and telephony module,
# our programs in place of its application). See README.md.
#   make                  the line helper
#   make image BASE=F.fw  build/v90d-obi200.fw from the owner's firmware file F
ARM_CC  ?= $(wildcard $(CURDIR)/../../toolchain/*/bin/arm-linux-gcc)
CFLAGS  ?= -std=gnu99 -O2 -Wall -Wextra -Werror
DRV     := ../drv
BUILD   := build
VERSION := $(shell cat ../VERSION)
.PHONY: all image test clean
all: $(BUILD)/obiline $(BUILD)/obinv
$(BUILD):
	@mkdir -p $(BUILD)
$(BUILD)/obiline: obiline.c compat26.c $(DRV)/pump.c $(DRV)/proslic.c $(DRV)/slic.c $(wildcard $(DRV)/*.h) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -pthread -I$(DRV) -o $@ obiline.c compat26.c $(DRV)/pump.c $(DRV)/proslic.c $(DRV)/slic.c
	$(dir $(ARM_CC))arm-linux-strip $@
$(BUILD)/obinv: obinv.c compat26.c ../../ppp/ppp_md5.c ../../cfg/page.h ../../cfg/sbuf.h | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -I../../ppp -I../../cfg -o $@ obinv.c compat26.c ../../ppp/ppp_md5.c
	$(dir $(ARM_CC))arm-linux-strip $@
../../srv/build/v90d-obi: FORCE
	$(MAKE) -C ../../srv v90d-obi
FORCE:
image: $(BUILD)/obiline $(BUILD)/obinv ../../srv/build/v90d-obi obi udhcpc.script ../v90d.conf ../VERSION mkobifw.py
	@test -n "$(BASE)" || { echo "make image BASE=your-OBi-firmware.fw"; exit 1; }
	@rm -rf $(BUILD)/image && mkdir -p $(BUILD)/image
	cp obi udhcpc.script $(BUILD)/obiline $(BUILD)/obinv ../v90d.conf $(BUILD)/image/
	cp ../../srv/build/v90d-obi $(BUILD)/image/v90d
	cp ../VERSION $(BUILD)/image/VERSION
	python3 mkobifw.py --full --files $(BUILD)/image "$(BASE)" $(BUILD)/v90d-obi200.fw
# obinv and the builder on the host, against a stand-in flash (needs BASE and the image)
$(BUILD)/obinv.host: obinv.c ../../ppp/ppp_md5.c ../../cfg/page.h ../../cfg/sbuf.h | $(BUILD)
	cc -std=gnu99 -O2 -Wall -Wextra -Werror -I../../ppp -I../../cfg -o $@ obinv.c ../../ppp/ppp_md5.c
test: $(BUILD)/obinv.host image
	python3 test_obinv.py $(BUILD)/obinv.host "$(BASE)" $(BUILD)/v90d-obi200.fw
clean:
	rm -rf $(BUILD)
