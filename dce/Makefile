# v90d/dce - one answered call end to end: modem + V.42 + PPP glued together.
# Host tests and ARM cross tests under qemu. The sources of the three
# libraries are compiled straight from their directories.
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

INC     := -I. -Itest -I../test -I../modem -I../v90 -I../v92 -I../v34 -I../sim -I../v42 -I../ppp -I../ppp/test

# every modem source: the answer modem keeps growing new parts (V.90 phases),
# and with V.90 it needs the PCM transmitter (v90/) and the V.34 upstream
# receiver (v34/) as well; with V.92 the PCM upstream receiver (v92/)
MODEM_SRC := $(wildcard ../modem/*.c) $(wildcard ../v90/*.c) $(wildcard ../v34/*.c) $(wildcard ../v32/*.c) $(wildcard ../v92/*.c)
V42_SRC := $(addprefix ../v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c)
PPP_SRC := $(addprefix ../ppp/,ppp_md5.c ppp_hdlc.c ppp_fsm.c ppp_deflate.c ppp_mppc.c ppp.c)
# the product: everything that would be linked into the daemon
LIB_SRC := dce.c $(MODEM_SRC) $(V42_SRC) $(PPP_SRC)
# test side only: channel simulator, calling modem, PPP client, calling DCE
SIM_SRC := $(addprefix ../sim/,g711.c noise.c filt.c channel.c v8call.c callpeer.c \
             p2a.c qammod.c p34a.c p2c.c v34peer.c v32call.c v32line.c resamp.c \
             v92a.c v92chan.c v92call.c) ../v90/test/v90_dec.c
SUP_SRC := test/harness.c test/caller.c ../ppp/test/pppc.c $(SIM_SRC)
HDRS    := dce.h test/harness.h test/caller.h ../test/test.h ../ppp/test/pppc.h \
           $(wildcard ../modem/*.h ../sim/*.h ../v42/*.h ../ppp/*.h ../v90/*.h ../v34/*.h ../v92/*.h ../v90/test/*.h)
TESTS   := test_call

.PHONY: all test test-arm test-findings clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(SUP_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) $(SUP_SRC) $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same test for the ARM926 target and run it under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(SUP_SRC) $(HDRS) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC) $(SUP_SRC) $(LDLIBS)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t $(ARM_ARGS) || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# not part of "test": documents open problems in ../modem ../v42 ../ppp
test-findings: $(BUILD)/test_findings
	@echo "== known problems (every FAIL below is an open finding) =="
	./$(BUILD)/test_findings

clean:
	rm -rf $(BUILD)
