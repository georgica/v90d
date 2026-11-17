# libv90modem: build, test, install.
#
#   make                 static and shared library, pkg-config file (in $(BUILD))
#   make check           the simulator suite on the host
#   make check-arm       the same, cross-built (ARM_CC) and run under qemu-arm
#   make install         into $(DESTDIR)$(PREFIX); make uninstall takes it out
#   make examples        examples/answer_file
#   make bench-v90       record a simulated V.90 call, replay it on the host and
#                        under qemu, through the internals and the public API
#   make test-interop    against spandsp (SPANDSP=its install prefix), host only
#
# Cross-compiling: set CC (and BUILD to keep the objects apart), e.g.
#   make CC=arm-linux-gcc BUILD=build/arm
# ar, ld and objcopy are taken from the same prefix as CC.
#
# Signal-path code is portable C99, fixed-point. The simulator (sim/) is a
# test peer and may use double; it is never installed.

NAME     := libv90modem
VERSION  := 0.1.0
SOVER    := 0

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Werror
# -D options for the modem code (bench experiments, trace builds)
EXTRA_CPPFLAGS ?=
CPPFLAGS_ALL := -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS)
LDLIBS   ?= -lm
BUILD    ?= build

# binutils next to the compiler: arm-linux-gcc -> arm-linux-ar ...
CROSS    := $(if $(filter %-gcc,$(notdir $(CC))),$(patsubst %gcc,%,$(CC)),)
AR       := $(CROSS)ar
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy

PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
INCDIR   ?= $(PREFIX)/include
BINDIR   ?= $(PREFIX)/bin
PCDIR    ?= $(LIBDIR)/pkgconfig

ARM_CC   ?= $(firstword $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc) arm-linux-gcc)
QEMU     ?= qemu-arm-static
SPANDSP  ?= $(CURDIR)/../peers/spandsp

INC      := -Iinclude -Iapi -Imodem -Iv90 -Iv92 -Iv34 -Iv32 -Iv42 -Isim

# ---- the library ----
CORE_SRC := $(wildcard modem/*.c) $(wildcard v90/*.c) $(wildcard v34/*.c) $(wildcard v32/*.c) $(wildcard v92/*.c) \
            $(addprefix v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c)
API_SRC  := api/v90modem.c
LIB_SRC  := $(API_SRC) $(CORE_SRC)
LIB_HDR  := include/v90modem.h api/v90modem_priv.h $(wildcard modem/*.h v90/*.h v34/*.h v32/*.h v92/*.h v42/*.h)
LIB_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(LIB_SRC))
PIC_OBJ  := $(patsubst %.c,$(BUILD)/pic/%.o,$(LIB_SRC))

STATIC   := $(BUILD)/$(NAME).a
SHARED   := $(BUILD)/$(NAME).so.$(VERSION)
PCFILE   := $(BUILD)/$(NAME).pc
# never installed: every internal symbol visible, plus the simulator; for the
# test suite, the tools here, and for programs that test against the
# simulated callers (v90d's call tests)
DEV      := $(BUILD)/$(NAME)-dev.a

# ---- the simulator and the test suite ----
SIM_SRC  := sim/g711.c sim/noise.c sim/filt.c sim/resamp.c sim/channel.c sim/v8call.c sim/callpeer.c sim/p2a.c sim/qammod.c \
            sim/p34a.c v90/test/v90_dec.c sim/p2c.c sim/v34peer.c sim/v32call.c sim/v32line.c sim/v92chan.c sim/v92a.c sim/v92call.c
SIM_OBJ  := $(patsubst %.c,$(BUILD)/obj/%.o,$(SIM_SRC))
MODEM_SRC := modem/v21.c modem/ansam.c modem/v8msg.c modem/async.c modem/v8rx.c modem/v8ans.c \
             modem/scramble.c modem/v22tx.c modem/v22rx.c modem/v22bis.c modem/v14.c modem/answer.c \
             modem/info.c modem/dpsk.c modem/probe.c modem/p2d.c modem/qamrx.c modem/ec.c modem/v34train.c modem/p34d.c modem/pcmenc.c \
             modem/v34mp.c modem/p2ans.c modem/v34mod.c modem/v34dx.c modem/v34ans.c \
             $(wildcard v90/*.c) $(wildcard v34/*.c) $(wildcard v32/*.c) $(wildcard v92/*.c)
T_SRC    := $(SIM_SRC) $(MODEM_SRC)
HDRS     := $(wildcard modem/*.h sim/*.h test/*.h v90/*.h v34/*.h v92/*.h v90/test/v90_dec.h)
T_CFLAGS := $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(EXTRA_CPPFLAGS) -Iv90 -Imodem -Isim -Iv92 -Iv34
TESTS    := test_g711 test_noise test_filt test_resamp test_channel test_v21 test_ansam test_v8msg test_v8rx test_v8ans \
            test_v22tx test_v22rx test_v22bis test_v14 test_answer test_info test_dpsk test_probe test_p2 test_qamrx test_ec test_v34train test_sres test_answer_v90 test_p34 test_pcmenc \
            test_v34mp test_p2ans test_v34mod test_v34dx test_v34call test_answer_v34 test_answer_v32 test_answer_v90_v34 \
            test_p2_v92 test_v92call test_answer_v92 test_v92real test_answer_lin
# the public API, against the library as it is installed
API_TESTS := test_api
SUBS     := v34 v90 v92 v42 v32

.PHONY: test-fixtures all lib dev install uninstall clean check check-arm test test-arm test-api test-interop examples bench-v90 bench-v34 replay
all: lib
lib: $(STATIC) $(SHARED) $(PCFILE)
dev: $(DEV)

$(BUILD)/obj/%.o: %.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -fvisibility=hidden $(INC) -c -o $@ $<
$(BUILD)/pic/%.o: %.c $(LIB_HDR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -fvisibility=hidden -fPIC $(INC) -c -o $@ $<

# Static: one relocatable object in which everything but the public API is
# made local, so that the internals cannot clash with a program's symbols.
$(STATIC): $(LIB_OBJ)
	$(LD) -r -o $(BUILD)/$(NAME).o $(LIB_OBJ)
	$(OBJCOPY) --localize-hidden $(BUILD)/$(NAME).o
	rm -f $@ && $(AR) rcs $@ $(BUILD)/$(NAME).o
$(SHARED): $(PIC_OBJ)
	$(CC) -shared -Wl,-soname,$(NAME).so.$(SOVER) -o $@ $(PIC_OBJ) $(LDLIBS)
	ln -sf $(NAME).so.$(VERSION) $(BUILD)/$(NAME).so.$(SOVER)
	ln -sf $(NAME).so.$(SOVER) $(BUILD)/$(NAME).so
$(PCFILE): $(NAME).pc.in Makefile
	@mkdir -p $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCDIR@|$(INCDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@
$(DEV): $(LIB_OBJ) $(SIM_OBJ)
	rm -f $@ && $(AR) rcs $@ $(LIB_OBJ) $(SIM_OBJ)

install: lib $(BUILD)/v90m-replay
	install -d $(DESTDIR)$(INCDIR) $(DESTDIR)$(LIBDIR) $(DESTDIR)$(PCDIR) $(DESTDIR)$(BINDIR)
	install -m 644 include/v90modem.h $(DESTDIR)$(INCDIR)/
	install -m 644 $(STATIC) $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SHARED) $(DESTDIR)$(LIBDIR)/
	ln -sf $(NAME).so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(NAME).so.$(SOVER)
	ln -sf $(NAME).so.$(SOVER) $(DESTDIR)$(LIBDIR)/$(NAME).so
	install -m 644 $(PCFILE) $(DESTDIR)$(PCDIR)/
	install -m 755 $(BUILD)/v90m-replay $(DESTDIR)$(BINDIR)/
uninstall:
	rm -f $(DESTDIR)$(INCDIR)/v90modem.h $(DESTDIR)$(LIBDIR)/$(NAME).a $(DESTDIR)$(LIBDIR)/$(NAME).so \
	      $(DESTDIR)$(LIBDIR)/$(NAME).so.$(SOVER) $(DESTDIR)$(LIBDIR)/$(NAME).so.$(VERSION) \
	      $(DESTDIR)$(PCDIR)/$(NAME).pc $(DESTDIR)$(BINDIR)/v90m-replay

# ---- tests: each test_X links test/test_X.c with the sources, as always ----
$(BUILD)/test_%: test/test_%.c $(T_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ $< $(T_SRC) $(LDLIBS)
$(BUILD)/arm_test_%: test/test_%.c $(T_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ $< $(T_SRC) $(LDLIBS)
# The API test: the answering modem from the static library as installed
# (only v90m_* visible), the simulated caller from the dev archive. Both
# carry the modem's code; the static library's copy is local to it.
API_INC := -Iinclude -Itest -Isim -Imodem -Iv90 -Iv92 -Iv34
$(BUILD)/api_test_%: test/api/test_%.c $(STATIC) $(DEV) include/v90modem.h
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -o $@ $< $(STATIC) $(DEV) $(LDLIBS)
$(BUILD)/arm/api_test_%: test/api/test_%.c include/v90modem.h
	$(MAKE) -s BUILD=$(BUILD)/arm CC=$(ARM_CC) $(BUILD)/arm/$(NAME).a $(BUILD)/arm/$(NAME)-dev.a
	$(ARM_CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L $(API_INC) -static -o $@ $< $(BUILD)/arm/$(NAME).a $(BUILD)/arm/$(NAME)-dev.a $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi
test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) $(abspath $(BUILD))/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi
test-api: $(addprefix $(BUILD)/api_,$(API_TESTS))
	@fail=0; for t in $(API_TESTS); do $(abspath $(BUILD))/api_$$t || fail=1; done; [ $$fail -eq 0 ]
test-api-arm: $(addprefix $(BUILD)/arm/api_,$(API_TESTS))
	@fail=0; for t in $(API_TESTS); do $(QEMU) $(abspath $(BUILD))/arm/api_$$t || fail=1; done; [ $$fail -eq 0 ]

check: test test-api test-fixtures
	@for d in $(SUBS); do echo "== $$d =="; $(MAKE) -s -C $$d BUILD=build test || exit 1; done; echo "ALL HOST CHECKS PASS"
# recordings of real calls (test/fixtures/fixtures.txt) through the replay tool
test-fixtures: $(BUILD)/v90m-replay
	@echo "== real-modem recordings =="; test/fixtures/check.sh $(abspath $(BUILD))/v90m-replay
check-arm: test-arm test-api-arm
	@for d in $(SUBS); do echo "== $$d (arm) =="; $(MAKE) -s -C $$d BUILD=build test-arm ARM_CC=$(ARM_CC) || exit 1; done; echo "ALL ARM CHECKS PASS"

INTEROP := interop_v8_spandsp interop_v22bis_spandsp
$(BUILD)/interop_%: test/interop_%.c test/peer_spandsp.c $(T_SRC)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -Wno-unused-parameter -I$(SPANDSP)/include -o $@ $< test/peer_spandsp.c $(T_SRC) -L$(SPANDSP)/lib -Wl,-rpath,$(SPANDSP)/lib -lspandsp -ltiff $(LDLIBS)
test-interop: $(addprefix $(BUILD)/,$(INTEROP))
	@echo "== running interop tests =="
	@fail=0; for t in $(INTEROP); do $(abspath $(BUILD))/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL INTEROP TESTS PASS"; else echo "INTEROP TESTS FAILED"; exit 1; fi

# ---- the example: answer a recorded call through the public API ----
examples: $(BUILD)/answer_file
$(BUILD)/answer_file: examples/answer_file.c $(STATIC) include/v90modem.h
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(STATIC) $(LDLIBS)

# ---- the replay tool (ships with the library): modem and V.42 only, built
# from the sources with the trace hooks in ----
REPLAY_SRC := tools/v90m_replay.c tools/trace.c $(LIB_SRC)
$(BUILD)/v90m-replay: $(REPLAY_SRC) $(LIB_HDR) tools/trace.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS_ALL) -DP34D_TRACE $(INC) -Itools -o $@ $(REPLAY_SRC) $(LDLIBS)
replay: $(BUILD)/v90m-replay

# ---- V.90 on the target: record a simulated call on the host, replay it
# (rig/bench_v90.c): CPU cost per phase and a bit-exactness check; the same
# recording through the public API (rig/bench_api.c), static and shared ----
V90_REC := $(BUILD)/v90_call.rec
bench-v90: $(BUILD)/rec_v90 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm $(BUILD)/bench_api $(BUILD)/bench_api.so $(BUILD)/bench_api.arm
	$(abspath $(BUILD))/rec_v90 -55 0.2 40 0 $(V90_REC) | tail -2
	$(abspath $(BUILD))/bench_v90 $(V90_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v90.arm $(V90_REC) | head -1
	$(abspath $(BUILD))/bench_api $(V90_REC)
	LD_LIBRARY_PATH=$(BUILD) $(abspath $(BUILD))/bench_api.so $(V90_REC)
	$(QEMU) $(abspath $(BUILD))/bench_api.arm $(V90_REC)
$(BUILD)/rec_v90: tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90: rig/bench_v90.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_v90.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90.arm: rig/bench_v90.c $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_v90.c $(MODEM_SRC) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@
$(BUILD)/bench_api: rig/bench_api.c $(STATIC)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< $(STATIC) $(LDLIBS)
$(BUILD)/bench_api.so: rig/bench_api.c $(SHARED)
	$(CC) $(CFLAGS) -Iinclude -o $@ $< -L$(BUILD) -lv90modem $(LDLIBS)
$(BUILD)/bench_api.arm: rig/bench_api.c include/v90modem.h
	$(MAKE) -s BUILD=$(BUILD)/arm CC=$(ARM_CC) $(BUILD)/arm/$(NAME).a
	$(ARM_CC) $(CFLAGS) -Iinclude -static -o $@ $< $(BUILD)/arm/$(NAME).a $(LDLIBS)

# ---- V.34 answering: record a simulated call, replay it on host and ARM ----
V34_REC ?= $(BUILD)/v34_call.rec
# cost-v34 counts ARM instructions per second of data mode for V.34 and, the
# same way, for V.90 (whose data mode was timed on the real box), so the two
# can be compared. Slow (minutes): qemu logs every instruction.
CNT = $(QEMU) -one-insn-per-tb -d exec,nochain
.PHONY: cost-v34
cost-v34: bench-v34 bench-v90
	@head -c $$((2 * 8000 * 10)) $(V34_REC) > $(BUILD)/v34_a.rec; head -c $$((2 * 8000 * 11)) $(V34_REC) > $(BUILD)/v34_b.rec; \
	 head -c $$((8000 * 10)) $(V90_REC) > $(BUILD)/v90_a.rec; head -c $$((8000 * 11)) $(V90_REC) > $(BUILD)/v90_b.rec; \
	 a=$$($(CNT) $(abspath $(BUILD))/bench_v34.arm $(BUILD)/v34_a.rec 2>&1 | grep -c '^Trace'); \
	 b=$$($(CNT) $(abspath $(BUILD))/bench_v34.arm $(BUILD)/v34_b.rec 2>&1 | grep -c '^Trace'); \
	 c=$$($(CNT) $(abspath $(BUILD))/bench_v90.arm $(BUILD)/v90_a.rec 2>&1 | grep -c '^Trace'); \
	 d=$$($(CNT) $(abspath $(BUILD))/bench_v90.arm $(BUILD)/v90_b.rec 2>&1 | grep -c '^Trace'); \
	 echo "V.34 data mode: $$((b - a)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "V.90 data mode: $$((d - c)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "ratio V.34 / V.90: $$(( (b - a) * 100 / (d - c) )) %"
bench-v34: $(BUILD)/rec_v34 $(BUILD)/bench_v34 $(BUILD)/bench_v34.arm
	$(abspath $(BUILD))/rec_v34 16 $(V34_REC)
	$(abspath $(BUILD))/bench_v34 $(V34_REC)
	$(QEMU) $(abspath $(BUILD))/bench_v34.arm $(V34_REC) | head -1
$(BUILD)/rec_v34: tools/rec_v34.c rig/v34glue.h $(SIM_SRC) $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ tools/rec_v34.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(T_CFLAGS) -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34.arm: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(T_CFLAGS) -static -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)

clean:
	rm -rf $(BUILD)
	@for d in $(SUBS); do $(MAKE) -s -C $$d BUILD=build clean; done
