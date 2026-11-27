# v90d: build system (host x86 tests first; ARM target cross-build later).
#
# The modem (V.8 to V.92, V.42) is libv90modem (libv90m.mk says where it is
# and how it is built). This tree is the box around it: telephony, the call
# (dce: modem + V.42 + PPP), the server, configuration, firmware.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS  ?= -lm
BUILD   := build

# Cross toolchain for the ARM926 target (kernel 3.4, 32-bit time_t).
ARM_CC  ?= $(wildcard $(CURDIR)/toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

include libv90m.mk

# rig/modemterm: the answering modem with a tiny terminal (rig/sipmodem.c);
# its test calls it with the library's simulated caller
TESTS   := test_modemterm

.PHONY: all test test-arm rig clean check check-arm check-ns bench-v90 bench-v34 cost-v34 test-interop
all: test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c rig/modemterm.c rig/modemterm.h $(LIBV90M_DEV) | $(BUILD)
	$(CC) $(CFLAGS) -Itest $(LIBV90M_DEV_INC) -o $@ $< rig/modemterm.c $(LIBV90M_DEV) $(LDLIBS)
$(BUILD)/arm_test_%: test/test_%.c rig/modemterm.c rig/modemterm.h $(LIBV90M_DEV_ARM) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -Itest $(LIBV90M_DEV_INC) -static -o $@ $< rig/modemterm.c $(LIBV90M_DEV_ARM) $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi
test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# real-modem test rig for the stock firmware's SIP/RTP path (see rig/README.md)
RIG_ALL := rig/sipmodem.c rig/modemterm.c rig/pppcall.c dce/dce.c \
           $(addprefix ppp/,ppp_md5.c ppp_crypto.c ppp_hdlc.c ppp_fsm.c ppp_deflate.c ppp_mppc.c ppp_mppe.c ppp_vj.c ppp.c)
RIG_INC := -Irig -Idce -Ippp $(LIBV90M_INC)
rig: $(BUILD)/sipmodem.x86 $(BUILD)/sipmodem.arm
$(BUILD)/sipmodem.x86: $(RIG_ALL) $(LIBV90M_A) | $(BUILD)
	$(CC) $(CFLAGS) $(RIG_INC) -o $@ $(RIG_ALL) $(LIBV90M_A) $(LDLIBS)
$(BUILD)/sipmodem.arm: $(RIG_ALL) $(LIBV90M_A_ARM) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(RIG_INC) -static -Os -o $@ $(RIG_ALL) $(LIBV90M_A_ARM) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@

clean:
	rm -rf $(BUILD)

# ---- everything: the library's suite, then every sub-project of the box ----
SUBS := ppp cfg tel dce srv arch
LIB_MAKE = $(MAKE) -s -C $(LIBV90M) BUILD=$(LIBV90M_OUT)/host ARM_CC=$(ARM_CC) QEMU=$(QEMU)
check:
	@echo "== libv90modem =="; $(LIB_MAKE) check
	@$(MAKE) -s test
	@for d in $(SUBS); do echo "== $$d =="; $(MAKE) -s -C $$d test || exit 1; done; echo "ALL HOST CHECKS PASS"
check-arm:
	@echo "== libv90modem (arm) =="; $(LIB_MAKE) check-arm
	@$(MAKE) -s test-arm
	@for d in $(SUBS); do echo "== $$d (arm) =="; $(MAKE) -s -C $$d test-arm || exit 1; done; echo "ALL ARM CHECKS PASS"
# the real daemon in a private network namespace (TUN, NAT, HTTP)
check-ns:
	@$(MAKE) -s -C srv test-ns
# interop against independent implementations (host only; peers are never shipped)
test-interop:
	$(LIB_MAKE) SPANDSP=$(CURDIR)/peers/spandsp test-interop

# ---- the modem's benches live with the library ----
bench-v90 bench-v34 cost-v34:
	$(LIB_MAKE) $@
