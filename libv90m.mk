# libv90modem, as v90d builds against it. Included by the Makefiles that
# need the modem (srv, dce, fw/drv, the top level, live/replay.mk).
#
#   LIBV90M       the library's source tree. The default is the pinned copy
#                 fw/setup.sh unpacks at $(V90D_ROOT)/libv90modem; point it
#                 at a checkout of the repository (LIBV90M=../libv90modem) to
#                 build against a working copy. Tests and tools need the
#                 source: they link the simulated callers and look at the
#                 modem's internals.
#   LIBV90M_DEFS  -D options for the modem code (live/replay's trace hooks,
#                 the bench experiments of live/*.sh); each set of them gets
#                 a build directory of its own.
#
# The daemon links the library as it is installed: libv90modem.a with only
# the public API visible, include/v90modem.h. To link an installed copy
# instead of building one from LIBV90M:
#   make LIBV90M_A=/usr/lib/libv90modem.a LIBV90M_INC="$(pkg-config --cflags libv90modem)"

V90D_ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LIBV90M      ?= $(abspath $(V90D_ROOT)/libv90modem)
LIBV90M_DEFS ?=
LIBV90M_TAG  := $(subst =,_,$(subst $() ,-,$(strip $(patsubst -D%,%,$(LIBV90M_DEFS)))))
LIBV90M_OUT  ?= $(V90D_ROOT)/build/libv90modem$(if $(LIBV90M_TAG),-$(LIBV90M_TAG))

LIBV90M_INC      ?= -I$(LIBV90M)/include
LIBV90M_A        ?= $(LIBV90M_OUT)/host/libv90modem.a
# the daemon on the box is built for size (-Os), as it always was
LIBV90M_A_ARM    ?= $(LIBV90M_OUT)/arm-os/libv90modem.a
# test side: every internal header and symbol, and the simulator
LIBV90M_DEV_INC  := $(LIBV90M_INC) $(addprefix -I$(LIBV90M)/,api modem v90 v92 v34 v32 v42 sim v90/test)
LIBV90M_DEV      := $(LIBV90M_OUT)/host/libv90modem-dev.a
LIBV90M_DEV_ARM  := $(LIBV90M_OUT)/arm/libv90modem-dev.a

LIBV90M_ARM_CC   ?= $(wildcard $(V90D_ROOT)/toolchain/*/bin/arm-linux-gcc)

ifeq ($(wildcard $(LIBV90M)/include/v90modem.h),)
$(error libv90modem is not at $(LIBV90M): run fw/setup.sh (or fw/setup-libv90m.sh for the PC tests alone) to fetch it, or build against a checkout with LIBV90M=/path/to/libv90modem)
endif

# The library's own Makefile decides what is out of date.
.PHONY: .libv90m-force
.libv90m-force:
$(LIBV90M_OUT)/host/%.a: .libv90m-force
	@$(MAKE) -s -C $(LIBV90M) BUILD=$(LIBV90M_OUT)/host EXTRA_CPPFLAGS="$(LIBV90M_DEFS)" $@
$(LIBV90M_OUT)/arm/%.a: .libv90m-force
	@$(MAKE) -s -C $(LIBV90M) BUILD=$(LIBV90M_OUT)/arm CC=$(LIBV90M_ARM_CC) EXTRA_CPPFLAGS="$(LIBV90M_DEFS)" $@
$(LIBV90M_OUT)/arm-os/%.a: .libv90m-force
	@$(MAKE) -s -C $(LIBV90M) BUILD=$(LIBV90M_OUT)/arm-os CC=$(LIBV90M_ARM_CC) CFLAGS="-std=c99 -Os -Wall -Wextra -Werror" EXTRA_CPPFLAGS="$(LIBV90M_DEFS)" $@
