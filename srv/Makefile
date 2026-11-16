# v90d/srv - the server core (telephony + call + status + configuration) and
# the Linux daemon around it.
#   make test       whole calls against the simulated caller, on the host
#   make test-arm   the same cross-built, under qemu
#   make v90d       the daemon for the host;  make v90d-arm  for the box
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
LDLIBS  ?= -lm
BUILD   := build
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static

INC := -I. -I../tel -I../dce -I../dce/test -I../cfg -I../test -I../modem -I../v90 -I../v92 -I../v34 -I../sim -I../v42 -I../ppp -I../ppp/test
MODEM_SRC := $(wildcard ../modem/*.c) $(wildcard ../v90/*.c) $(wildcard ../v34/*.c) $(wildcard ../v32/*.c) $(wildcard ../v92/*.c)
V42_SRC := $(addprefix ../v42/,v42_hdlc.c v42_detect.c v42_comp.c v44_comp.c v42_lapm.c v42.c)
PPP_SRC := $(addprefix ../ppp/,ppp_md5.c ppp_hdlc.c ppp_fsm.c ppp_deflate.c ppp_mppc.c ppp.c)
CFG_SRC := ../cfg/config.c ../cfg/status.c
# everything portable that goes into the daemon
CORE_SRC := srv.c console.c dclist.c dcnow.c upnp.c line.c ../tel/tel.c ../dce/dce.c $(MODEM_SRC) $(V42_SRC) $(PPP_SRC) $(CFG_SRC)
SIM_SRC := $(addprefix ../sim/,g711.c noise.c filt.c channel.c v8call.c callpeer.c p2a.c qammod.c p34a.c p2c.c v34peer.c v32call.c v32line.c resamp.c v92a.c v92chan.c v92call.c) \
           ../v90/test/v90_dec.c ../dce/test/caller.c ../ppp/test/pppc.c
HDRS := $(wildcard *.h ../tel/*.h ../dce/*.h ../dce/test/*.h ../cfg/*.h ../modem/*.h ../v90/*.h ../v34/*.h ../v92/*.h ../v42/*.h ../ppp/*.h ../sim/*.h)

DAEMON_SRC := main.c tun.c nat.c ../cfg/httpd.c ../cfg/settings.c ../cfg/firmware.c $(CORE_SRC)
# the firmware's version, shown on the pages: one place, fw/VERSION
VERSION := $(shell cat ../fw/VERSION 2>/dev/null || echo dev)
DAEMON_DEF := -DV90D_VERSION='"$(VERSION)"'
NS_SRC := test/ns_call.c tun.c $(filter-out srv.c ../tel/tel.c ../dce/dce.c,$(CORE_SRC)) $(SIM_SRC)

RIG_SRC := ../rig/pppcall.c $(filter-out srv.c ../tel/tel.c $(CFG_SRC),$(CORE_SRC)) $(SIM_SRC)
$(BUILD)/test_rig: test/test_rig.c $(RIG_SRC) $(HDRS) ../rig/pppcall.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -I../rig -o $@ test/test_rig.c $(RIG_SRC) $(LDLIBS)
$(BUILD)/arm_test_rig: test/test_rig.c $(RIG_SRC) $(HDRS) ../rig/pppcall.h
	@mkdir -p $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -I../rig -static -o $@ test/test_rig.c $(RIG_SRC) $(LDLIBS)

.PHONY: test test-arm test-ns test-ns-nat test-ns-upnp test-ns-pppd v90d v90d-arm clean
v90d: $(BUILD)/v90d
v90d-arm: $(BUILD)/v90d-arm
$(BUILD)/v90d: $(DAEMON_SRC) $(HDRS) ../fw/VERSION
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(DAEMON_DEF) $(INC) -o $@ $(DAEMON_SRC) $(LDLIBS)
$(BUILD)/v90d-arm: $(DAEMON_SRC) $(HDRS) ../fw/VERSION
	@mkdir -p $(BUILD)
	$(ARM_CC) $(CFLAGS) $(DAEMON_DEF) $(INC) -static -Os -o $@ $(DAEMON_SRC) $(LDLIBS)
	$(dir $(ARM_CC))arm-linux-strip $@
# The real daemon in a private network namespace (needs unprivileged user
# namespaces): TUN device, NAT rule, HTTP listener, a whole call.
$(BUILD)/ns_call: $(NS_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(NS_SRC) $(LDLIBS)
# The real daemon against the real pppd (test/ns_pppd.c): PPP compression
# with a peer that was not written here. pppd needs /dev/ppp and the
# kernel's ppp_deflate, so this part runs as root (sudo -n) in namespaces of
# its own, and is left out, with a line that says so, where there is no
# pppd, no password-less sudo or no ppp_deflate module.
NSP_SRC := test/ns_pppd.c $(filter-out srv.c ../tel/tel.c ../dce/dce.c,$(CORE_SRC)) $(SIM_SRC)
$(BUILD)/ns_pppd: $(NSP_SRC) $(HDRS) ../ppp/test/gen.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $(NSP_SRC) $(LDLIBS)
test-ns-pppd: $(BUILD)/v90d $(BUILD)/ns_pppd
	@if ! command -v pppd >/dev/null 2>&1; then echo "  (no pppd here: the call with the real pppd is not tried)"; \
	 elif ! sudo -n true 2>/dev/null; then echo "  (no sudo without a password: the call with the real pppd is not tried)"; \
	 elif ! sudo -n modprobe ppp_deflate 2>/dev/null; then echo "  (no ppp_deflate module in this kernel: the call with the real pppd is not tried)"; \
	 else d=$$(mktemp -d) && { sudo -n unshare -n -m ./$(BUILD)/ns_pppd ./$(BUILD)/v90d $$d; rc=$$?; rm -rf $$d; exit $$rc; }; fi

# The Firmware page end to end takes the image's flash tool built for the
# host and a file that stands for the flash (fw/tools); without python3 or
# fw/ that part is left out.
FW_NV := ../fw/tools/v90nv.host
$(BUILD)/ns_nat: test/ns_nat.c nat.c nat.h tun.c tun.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test/ns_nat.c nat.c tun.c
test-ns-nat: $(BUILD)/ns_nat
	unshare -Urn ./$(BUILD)/ns_nat
$(BUILD)/ns_upnp: test/ns_upnp.c upnp.c upnp.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test/ns_upnp.c upnp.c
test-ns-upnp: $(BUILD)/ns_upnp
	unshare -Urn ./$(BUILD)/ns_upnp $(BUILD)

test-ns: $(BUILD)/v90d $(BUILD)/ns_call test-ns-nat test-ns-upnp
	@$(MAKE) -s -C ../fw tools/v90nv.host && python3 ../fw/tools/test_v90nv_boot.py --make-flash $(BUILD)/ns_flash.bin $(BUILD)/ns_new.uimage \
	  || echo "  (no flash tool or no python3: the Firmware page is not tried)"
	unshare -Urn ./$(BUILD)/ns_call ./$(BUILD)/v90d $(BUILD) $$(test -f $(BUILD)/ns_flash.bin && test -x $(FW_NV) && echo $(FW_NV) $(BUILD)/ns_flash.bin $(BUILD)/ns_new.uimage)
	@rm -f $(BUILD)/ns_flash.bin
	@$(MAKE) -s test-ns-pppd

test: $(BUILD)/test_srv $(BUILD)/test_rig
	./$(BUILD)/test_srv
	./$(BUILD)/test_rig
test-arm: $(BUILD)/arm_test_srv $(BUILD)/arm_test_rig
	$(QEMU) ./$(BUILD)/arm_test_srv
	$(QEMU) ./$(BUILD)/arm_test_rig

$(BUILD)/test_srv: test/test_srv.c $(CORE_SRC) $(SIM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ test/test_srv.c $(CORE_SRC) $(SIM_SRC) $(LDLIBS)
$(BUILD)/arm_test_srv: test/test_srv.c $(CORE_SRC) $(SIM_SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(CFLAGS) $(INC) -static -o $@ test/test_srv.c $(CORE_SRC) $(SIM_SRC) $(LDLIBS)
clean:
	rm -rf $(BUILD)
