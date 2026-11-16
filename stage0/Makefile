XCC    ?= $(HOME)/v90d/toolchain/armv5-eabi--uclibc--stable-2023.08-1/bin/arm-linux-gcc
CFLAGS  = -std=c99 -D_POSIX_C_SOURCE=200112L -O2 -Wall -Wextra
ARMFLAGS = -march=armv5te -mtune=arm926ej-s -mfloat-abi=soft -static

all: hello.arm bench.arm bench.x86 sipecho.arm sipecho.x86 pcmpat.x86 pcmpat.arm chsim pcman

%.arm: %.c
	$(XCC) $(CFLAGS) $(ARMFLAGS) -o $@ $<

%.x86: %.c
	$(CC) $(CFLAGS) -o $@ $<

chsim pcman: %: %.c g711.h pat.h
	$(CC) -std=c99 -D_POSIX_C_SOURCE=200112L -O2 -Wall -Wextra -o $@ $< -lm

clean:
	rm -f *.arm *.x86 chsim pcman
