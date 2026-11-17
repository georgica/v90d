# v90d: build system (host x86 tests first; ARM target cross-build later).
#
# Signal-path code (modem, telephony) is portable C99, fixed-point. The channel
# simulator (sim/) is an x86 test peer and may use double.

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -Iv90 -Imodem -Isim -Iv92 -Iv34
LDLIBS  ?= -lm
BUILD   := build

# Cross toolchain for the ARM926 target (kernel 3.4, 32-bit time_t).
ARM_CC  ?= $(wildcard $(CURDIR)/toolchain/*/bin/arm-linux-gcc)

SIM_SRC := sim/g711.c sim/noise.c sim/filt.c sim/resamp.c sim/channel.c sim/v8call.c sim/callpeer.c sim/p2a.c sim/qammod.c sim/p34a.c v90/test/v90_dec.c \
           sim/p2c.c sim/v34peer.c sim/v32call.c sim/v32line.c sim/v92chan.c sim/v92a.c sim/v92call.c
MODEM_SRC := modem/v21.c modem/ansam.c modem/v8msg.c modem/async.c modem/v8rx.c modem/v8ans.c \
             modem/scramble.c modem/v22tx.c modem/v22rx.c modem/v22bis.c modem/v14.c modem/answer.c \
             modem/info.c modem/dpsk.c modem/probe.c modem/p2d.c modem/qamrx.c modem/ec.c modem/ecdly.c modem/tonedet.c modem/v34train.c modem/p34d.c modem/pcmenc.c \
             modem/v34mp.c modem/p2ans.c modem/v34mod.c modem/v34dx.c modem/v34ans.c \
             $(wildcard v90/*.c) $(wildcard v34/*.c) $(wildcard v32/*.c) $(wildcard v92/*.c)
RIG_SRC := rig/modemterm.c
LIB_SRC := $(SIM_SRC) $(MODEM_SRC) $(RIG_SRC)
HDRS    := $(wildcard modem/*.h sim/*.h rig/*.h test/*.h v90/*.h v34/*.h v92/*.h v90/test/v90_dec.h)
TESTS   := test_g711 test_noise test_filt test_resamp test_channel test_v21 test_ansam test_v8msg test_v8rx test_v8ans \
           test_v22tx test_v22rx test_v22bis test_v14 test_answer test_modemterm test_info test_dpsk test_probe test_p2 test_qamrx test_ec test_ecdly test_v34train test_sres test_answer_v90 test_p34 test_pcmenc \
           test_v34mp test_p2ans test_v34mod test_v34dx test_v34call test_answer_v34 test_answer_v32 test_answer_v90_v34 \
           test_p2_v92 test_v92call test_answer_v92 test_v92real test_answer_lin

QEMU    ?= qemu-arm-static

.PHONY: all test test-arm test-interop rig clean
all: test

$(BUILD):
	@mkdir -p $(BUILD)

# each test_X links test/test_X.c with the sim/modem sources
$(BUILD)/test_%: test/test_%.c $(LIB_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LIB_SRC) $(LDLIBS)

test: $(addprefix $(BUILD)/,$(TESTS))
	@echo "== running host tests =="
	@fail=0; for t in $(TESTS); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL TESTS PASS"; else echo "TESTS FAILED"; exit 1; fi

# cross-build the same tests for the ARM926 target and run under qemu
$(BUILD)/arm_test_%: test/test_%.c $(LIB_SRC) $(HDRS) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -o $@ $< $(LIB_SRC) $(LDLIBS)

test-arm: $(addprefix $(BUILD)/arm_,$(TESTS))
	@echo "== running ARM tests under $(QEMU) =="
	@fail=0; for t in $(TESTS); do echo "[arm] $$t"; $(QEMU) ./$(BUILD)/arm_$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL ARM TESTS PASS"; else echo "ARM TESTS FAILED"; exit 1; fi

# interop against independent implementations (host only; peers are never shipped)
SPANDSP := $(CURDIR)/peers/spandsp
INTEROP := interop_v8_spandsp interop_v22bis_spandsp
$(BUILD)/interop_%: test/interop_%.c test/peer_spandsp.c $(LIB_SRC) | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -I$(SPANDSP)/include -o $@ $< test/peer_spandsp.c $(LIB_SRC) -L$(SPANDSP)/lib -Wl,-rpath,$(SPANDSP)/lib -lspandsp -ltiff $(LDLIBS)

test-interop: $(addprefix $(BUILD)/,$(INTEROP))
	@echo "== running interop tests =="
	@fail=0; for t in $(INTEROP); do ./$(BUILD)/$$t || fail=1; done; \
	 if [ $$fail -eq 0 ]; then echo "ALL INTEROP TESTS PASS"; else echo "INTEROP TESTS FAILED"; exit 1; fi

# real-modem test rig for the stock firmware's SIP/RTP path (see rig/README.md)
RIG_ALL := rig/sipmodem.c rig/modemterm.c rig/pppcall.c sim/g711.c $(MODEM_SRC) dce/dce.c \
           $(addprefix v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c) \
           $(addprefix ppp/,ppp_md5.c ppp_hdlc.c ppp_fsm.c ppp_deflate.c ppp_mppc.c ppp.c)
RIG_INC := -Irig -Imodem -Idce -Iv42 -Ippp
rig: $(BUILD)/sipmodem.x86 $(BUILD)/sipmodem.arm
$(BUILD)/sipmodem.x86: $(RIG_ALL) | $(BUILD)
	$(CC) $(CFLAGS) $(RIG_INC) -o $@ $(RIG_ALL) $(LDLIBS)
$(BUILD)/sipmodem.arm: $(RIG_ALL) | $(BUILD)
	$(ARM_CC) $(CFLAGS) $(RIG_INC) -static -Os -o $@ $(RIG_ALL) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@

clean:
	rm -rf $(BUILD)

# ---- everything: the top-level suite and every sub-project ----
SUBS := v34 v90 v92 v42 ppp cfg tel dce srv v32 arch
.PHONY: check check-arm check-ns
check: test
	@for d in $(SUBS); do echo "== $$d =="; $(MAKE) -s -C $$d test || exit 1; done; echo "ALL HOST CHECKS PASS"
check-arm: test-arm
	@for d in $(SUBS); do echo "== $$d (arm) =="; $(MAKE) -s -C $$d test-arm || exit 1; done; echo "ALL ARM CHECKS PASS"
# the real daemon in a private network namespace (TUN, NAT, HTTP)
check-ns:
	@$(MAKE) -s -C srv test-ns

# ---- V.90 on the target: record a simulated call on the host, replay it on
# the box (rig/bench_v90.c): CPU cost per phase and a bit-exactness check ----
V90_REC := $(BUILD)/v90_call.rec
.PHONY: bench-v90
bench-v90: $(BUILD)/rec_v90 $(BUILD)/bench_v90 $(BUILD)/bench_v90.arm
	./$(BUILD)/rec_v90 -55 0.2 40 0 $(V90_REC) | tail -2
	./$(BUILD)/bench_v90 $(V90_REC)
	$(QEMU) ./$(BUILD)/bench_v90.arm $(V90_REC) | head -1
$(BUILD)/rec_v90: tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -Imodem -Isim -o $@ tools/rec_v90.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90: rig/bench_v90.c $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ rig/bench_v90.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v90.arm: rig/bench_v90.c $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -o $@ rig/bench_v90.c $(MODEM_SRC) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@

# ---- V.34 answering: record a simulated call, replay it on host and ARM ----
# cost-v34 counts ARM instructions per second of data mode for V.34 and, the
# same way, for V.90 (whose data mode was timed on the real box), so the two
# can be compared. Slow (minutes): qemu logs every instruction.
V34_REC ?= $(BUILD)/v34_call.rec
CNT = $(QEMU) -one-insn-per-tb -d exec,nochain
.PHONY: bench-v34 cost-v34
bench-v34: $(BUILD)/rec_v34 $(BUILD)/bench_v34 $(BUILD)/bench_v34.arm
	./$(BUILD)/rec_v34 16 $(V34_REC)
	./$(BUILD)/bench_v34 $(V34_REC)
	$(QEMU) ./$(BUILD)/bench_v34.arm $(V34_REC) | head -1
cost-v34: bench-v34 bench-v90
	@head -c $$((2 * 8000 * 10)) $(V34_REC) > $(BUILD)/v34_a.rec; head -c $$((2 * 8000 * 11)) $(V34_REC) > $(BUILD)/v34_b.rec; \
	 head -c $$((8000 * 10)) $(V90_REC) > $(BUILD)/v90_a.rec; head -c $$((8000 * 11)) $(V90_REC) > $(BUILD)/v90_b.rec; \
	 a=$$($(CNT) ./$(BUILD)/bench_v34.arm $(BUILD)/v34_a.rec 2>&1 | grep -c '^Trace'); \
	 b=$$($(CNT) ./$(BUILD)/bench_v34.arm $(BUILD)/v34_b.rec 2>&1 | grep -c '^Trace'); \
	 c=$$($(CNT) ./$(BUILD)/bench_v90.arm $(BUILD)/v90_a.rec 2>&1 | grep -c '^Trace'); \
	 d=$$($(CNT) ./$(BUILD)/bench_v90.arm $(BUILD)/v90_b.rec 2>&1 | grep -c '^Trace'); \
	 echo "V.34 data mode: $$((b - a)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "V.90 data mode: $$((d - c)) ARM instructions per second of line (second 10 to 11 of the call)"; \
	 echo "ratio V.34 / V.90: $$(( (b - a) * 100 / (d - c) )) %"
$(BUILD)/rec_v34: tools/rec_v34.c rig/v34glue.h $(SIM_SRC) $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/rec_v34.c $(SIM_SRC) $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)
$(BUILD)/bench_v34.arm: rig/bench_v34.c rig/v34glue.h $(MODEM_SRC) $(HDRS) | $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -o $@ rig/bench_v34.c $(MODEM_SRC) $(LDLIBS)
