# v90d/dce - one answered call end to end: modem + V.42 + PPP glued together.
# Host tests and ARM cross tests under qemu. dce.c and PPP are compiled from
# their directories; the modem and V.42 come from libv90modem (../libv90m.mk).
#
#   make test       complete simulated calls on the host
#   make test-arm   the same test cross-built (-static) and run under qemu-arm,
#                   by default the shorter "quick" subset (ARM_ARGS= for all)
#   make test-findings   known problems in the libraries underneath, each a
#                   check that fails while the problem exists (exit status 1)

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS  ?= -lm
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static
ARM_ARGS ?= quick

include ../libv90m.mk

INC     := -I. -Itest -I../test -I../ppp -I../ppp/test $(LIBV90M_DEV_INC)

PPP_SRC := $(addprefix ../ppp/,ppp_md5.c ppp_crypto.c ppp_hdlc.c ppp_fsm.c ppp_deflate.c ppp_mppc.c ppp_mppe.c ppp_vj.c ppp.c)
# the product: everything that would be linked into the daemon (the modem
# and V.42 come from libv90modem)
LIB_SRC := dce.c $(PPP_SRC)
# test side only: calling DCE and PPP client; the simulated line and
# calling modem are in libv90modem's dev archive
SUP_SRC := test/harness.c test/caller.c ../ppp/test/pppc.c
HDRS    := dce.h test/harness.h test/caller.h ../test/test.h ../ppp/test/pppc.h $(wildcard ../ppp/*.h) $(LIBV90M)/include/v90modem.h
TESTS   := test_call test_orig

.PHONY: all test test-arm test-findings clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(SUP_SRC) $(HDRS) $(LIBV90M_DEV) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) $(SUP_SRC) $(LIBV90M_DEV) $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same test for the ARM926 target and run it under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(SUP_SRC) $(HDRS) $(LIBV90M_DEV_ARM) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC) $(SUP_SRC) $(LIBV90M_DEV_ARM) $(LDLIBS)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t $(ARM_ARGS) || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# not part of "test": documents open problems in libv90modem and ../ppp
test-findings: $(BUILD)/test_findings
	@echo "== known problems (every FAIL below is an open finding) =="
	./$(BUILD)/test_findings

clean:
	rm -rf $(BUILD)
