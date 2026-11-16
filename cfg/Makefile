# v90d/cfg - configuration loader and HTTP status server: host tests and
# ARM cross tests under qemu. Tests never open a TCP socket; the server is
# driven through socketpair() fds.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
BUILD   := build

# Cross toolchain for the ARM926 target (same one the top-level Makefile uses).
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -I../test
LIB_SRC := config.c status.c settings.c firmware.c httpd.c vpn.c
LIB_HDR := config.h status.h settings.h firmware.h httpd.h sbuf.h page.h vpn.h
SUP_HDR := ../test/test.h
TESTS   := test_config test_status test_settings test_firmware test_httpd test_vpn

.PHONY: all test test-arm clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) $(SUP_HDR) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

clean:
	rm -rf $(BUILD)
