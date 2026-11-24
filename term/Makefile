# v90d/term - the terminal menu (tscr, tart, guide, tdir, tmenu) and the
# v90d-bbsinfo tool: host tests and ARM cross tests under qemu.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
BUILD   := build
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC     := -I. -I../test -I../srv
LIB_SRC := tscr.c tart.c guide.c tdir.c tmenu.c
LIB_HDR := tscr.h tart.h guide.h tdir.h tmenu.h
TESTS   := test_tscr test_guide test_tdir test_tmenu

.PHONY: all test test-arm clean
all: test $(BUILD)/v90d-bbsinfo

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) ../test/test.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB_SRC) -lm

$(BUILD)/v90d-bbsinfo: bbsinfo.c guide.c guide.h ../srv/bbs.c ../srv/bbs.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ bbsinfo.c guide.c ../srv/bbs.c

test: $(addprefix $(BUILD)/,$(TESTS))
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(LIB_HDR) ../test/test.h | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ $< $(LIB_SRC) -lm

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@fail=0; for t in $(TESTS); do $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

clean:
	rm -rf $(BUILD)
