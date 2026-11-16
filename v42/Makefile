# v42/ build: ITU-T V.42 (LAPM) error control, V.42bis and V.44 compression.
# Portable C99, no floating point, no dynamic allocation, no OS calls.
#
#   make test          host unit and end-to-end tests
#   make test-arm      same tests cross-built (-static) and run under qemu-arm
#   make test-interop  host only: our side against spandsp as independent peer

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS  ?= -lm
ROOT    := $(abspath $(CURDIR)/..)
BUILD   := build
INC     := -I. -I$(ROOT)/test

ARM_CC  ?= $(wildcard $(ROOT)/toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

SRC     := v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c
HDR     := v42_hdlc.h v42_detect.h v42_comp.h v44_comp.h v42_lapm.h v42.h
TESTS   := test_hdlc test_detect test_comp test_v44 test_lapm test_v42

.PHONY: all test test-arm test-interop clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(SRC) $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running v42 host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

$(BUILD)/arm_test_%: test/test_%.c $(SRC) $(HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(SRC) $(LDLIBS)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running v42 ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# interop against spandsp (host only; the peer is never shipped). All spandsp
# includes live in test/peer_spandsp_v42.c, which exposes a plain C API.
SPANDSP := $(ROOT)/peers/spandsp
INTEROP := interop_v42_spandsp interop_v42bis_spandsp
$(BUILD)/interop_%: test/interop_%.c test/peer_spandsp_v42.c test/peer_spandsp_v42.h $(SRC) $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter $(INC) -I$(SPANDSP)/include -o $@ $< test/peer_spandsp_v42.c $(SRC) \
	  -L$(SPANDSP)/lib -Wl,-rpath,$(SPANDSP)/lib -lspandsp -ltiff $(LDLIBS)

test-interop: $(addprefix $(BUILD)/,$(INTEROP))
	@echo "== running v42 interop tests =="
	@fail=0; for t in $(INTEROP); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL INTEROP TESTS PASS"; else echo "INTEROP TESTS FAILED"; exit 1; fi

clean:
	rm -rf $(BUILD)
