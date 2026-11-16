# v90d/v34 - V.34 primary-channel encoder / decoder library (bits <-> 2D
# points): host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -I../test
LIB_SRC := v34_constel.c v34_core.c v34_params.c v34_shell.c v34_train.c \
           v34_tx.c v34_rx.c v34_nl.c
LIB_HDR := v34.h v34_priv.h
SUP_HDR := ../test/test.h test/fig5_data.h test/spec_tables.h test/testutil.h
TESTS   := test_scr test_params test_constel test_shell test_trellis \
           test_train test_roundtrip test_precoder test_ber test_resync \
           test_bench test_nl

.PHONY: all test test-arm cost-arm tables clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) -lm

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
# (the tests shorten their own Monte Carlo runs when built for ARM)
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC) -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# Exact ARM instruction counts per 2D symbol: qemu logs one line per executed
# instruction; decoder = (encoder + decoder) - (encoder), between two run
# lengths so that start-up cancels. The 128-tap FIR of stage0/bench.c is
# counted the same way and gives the cost in "rfir MACs", the kernel that
# was timed on the real box (25.4 MMAC/s, in units of 1000 MAC/s below).
# Fails if the 16-state decoder exceeds COST_LIMIT instructions per symbol.
# Slow (several minutes), so not part of test-arm.
COUNT      = $(QEMU) -one-insn-per-tb -d exec,nochain ./$(BUILD)/arm_test_bench count
NINSN      = 2>&1 | grep -c '^Trace'
BOX_KMAC   ?= 25400
COST_LIMIT ?= 3000
cost-arm: $(BUILD)/arm_test_bench
	@echo "== ARM instructions per 2D symbol (qemu -one-insn-per-tb) =="
	@r1=$$($(COUNT) 0 3 256 $(NINSN)); r2=$$($(COUNT) 0 3 768 $(NINSN)); \
	 mac100=$$(( (r2 - r1) * 100 / (512 * 128) )); fail=0; \
	 echo "  reference FIR: $$(( mac100 / 100 )).$$(printf %02d $$(( mac100 % 100 ))) instructions per MAC"; \
	 i=0; for name in "3200/4800  16 st" "3200/19200 16 st" "3200/31200 16 st" \
	    "3200/19200 16 st precoded" "3200/19200 32 st" "3200/19200 64 st"; do \
	  a1=$$($(COUNT) $$i 0 1024 $(NINSN)); a2=$$($(COUNT) $$i 0 3072 $(NINSN)); \
	  b1=$$($(COUNT) $$i 1 1024 $(NINSN)); b2=$$($(COUNT) $$i 1 3072 $(NINSN)); \
	  c1=$$($(COUNT) $$i 2 1024 $(NINSN)); c2=$$($(COUNT) $$i 2 3072 $(NINSN)); \
	  enc=$$(( (a2 - a1) / 2048 )); dec=$$(( (b2 - b1 - a2 + a1) / 2048 )); \
	  tent=$$(( (c2 - c1 - a2 + a1) / 2048 )); macs=$$(( dec * 100 / mac100 )); \
	  share=$$(( macs * 3200 / $(BOX_KMAC) )); \
	  echo "  $$name: encoder $$enc, decoder $$dec, with tentative decisions $$tent;" \
	       "decoder = $$macs MACs, about $$(( share / 10 )).$$(( share % 10 )) % of the box at 3200 sym/s"; \
	  if [ $$i -le 3 ] && [ $$dec -gt $(COST_LIMIT) ]; then echo "  OVER THE LIMIT of $(COST_LIMIT)"; fail=1; fi; \
	  i=$$((i + 1)); done; \
	 if [ $$fail -eq 0 ]; then echo "COST WITHIN LIMIT ($(COST_LIMIT) instructions per symbol, 16 states)"; else exit 1; fi

# regenerate v34_constel.c and test/fig5_data.h (needs python3; the outputs
# are checked in, so a normal build does not need this)
tables:
	python3 tools/gen_tables.py

clean:
	rm -rf $(BUILD)
