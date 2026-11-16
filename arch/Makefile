# v90d/arch - archive mode gateway (v90arch): synthetic DNS, HTTP relay and
# FTP gateway in front of the Protoweb proxy.
#   make test       unit tests and the end-to-end test (real sockets on
#                   127.x, a fake proxy, no Internet), on the host
#   make test-arm   the unit tests cross-built, under qemu
#   make test-arm-e2e  the end-to-end test against the ARM binary under qemu
#   make v90arch    the gateway for the host;  make v90arch-arm  for the box
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
BUILD   := build
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static
PYTHON  ?= python3

INC     := -I. -I../test
LIB_SRC := dns.c http.c listing.c ftp.c
LIB_HDR := dns.h http.h listing.h ftp.h
SUP_HDR := ../test/test.h
TESTS   := test_dns test_http test_listing test_ftp

.PHONY: all test test-unit test-e2e test-arm test-arm-e2e v90arch v90arch-arm clean
all: v90arch

v90arch: $(BUILD)/v90arch
v90arch-arm: $(BUILD)/v90arch-arm

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/v90arch: v90arch.c $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ v90arch.c $(LIB_SRC)
$(BUILD)/v90arch-arm: v90arch.c $(LIB_SRC) $(LIB_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -Os -o $@ v90arch.c $(LIB_SRC)
	$(dir $(ARM_CC))arm-linux-strip $@

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC)

test-unit: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL UNIT TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# the real program on high ports against a fake proxy; never leaves 127.x
test-e2e: $(BUILD)/v90arch
	@echo "== running the end-to-end test =="
	$(PYTHON) test/e2e.py ./$(BUILD)/v90arch

test: test-unit test-e2e
	@echo "ALL TESTS PASS"

# cross-build the same unit tests for the ARM926 target and run under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

test-arm-e2e: $(BUILD)/v90arch-arm
	$(PYTHON) test/e2e.py $(QEMU) ./$(BUILD)/v90arch-arm

clean:
	rm -rf $(BUILD)
