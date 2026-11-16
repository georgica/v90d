# fw/drv: the line driver. The parts that are plain C (the sample pump, the
# SLIC bring-up sequence) are tested here on the host and under qemu; the
# kernel glue is built with the kernel (see fw/README.md).
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
ARM_CC  ?= $(wildcard $(CURDIR)/../../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static
BUILD   := build
TESTS   := test_pump test_proslic test_slic test_qspi
SRC     := pump.c proslic.c slic.c qspi.c test/chipsim.c test/qspisim.c ../../v90/v90_pcm.c
.PHONY: test test-arm clean
$(BUILD):
	@mkdir -p $(BUILD)
$(BUILD)/%: test/%.c $(SRC) $(wildcard *.h test/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -I../../test -I../../v90 -I. -o $@ $< $(SRC) -lm
$(BUILD)/arm_%: test/%.c $(SRC) $(wildcard *.h test/*.h) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -I../../test -I../../v90 -I. -o $@ $< $(SRC) -lm
test: $(addprefix $(BUILD)/,$(TESTS))
	@for t in $(TESTS); do ./$(BUILD)/$$t || exit 1; done
test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@for t in $(TESTS); do $(QEMU) ./$(BUILD)/arm_$$t || exit 1; done
clean:
	rm -rf $(BUILD)
