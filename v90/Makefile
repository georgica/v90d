# v90d/v90 - V.90 digital modem transmit side (Phase 3 onwards) and the
# sequence codecs: host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -Itest -I../test
LIB_SRC := v90_pcm.c v90_scr.c v90_seq.c v90_enc.c v90_tx.c
LIB_HDR := v90_pcm.h v90_scr.h v90_seq.h v90_enc.h v90_tx.h
# test-only support: the reference decoder (never linked into the modem)
SUP_SRC := test/v90_dec.c
SUP_HDR := test/v90_dec.h test/table1.inc ../test/test.h
TESTS   := test_pcm test_scr test_seq test_enc test_tx test_tx92 test_e2e

.PHONY: all test test-arm clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# each test_X links test/test_X.c with the library and the reference decoder
$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_SRC) $(SUP_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) $(SUP_SRC) -lm

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_SRC) $(SUP_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC) $(SUP_SRC) -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

clean:
	rm -rf $(BUILD)
