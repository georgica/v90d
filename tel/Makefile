# v90d telephony layer. `make test` on the host, `make test-arm` under qemu.
CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
ARM_CC  ?= $(wildcard $(CURDIR)/../toolchain/*/bin/arm-linux-gcc)
QEMU    ?= qemu-arm-static
BUILD   := build
SRC     := tel.c
HDRS    := tel.h ../modem/sintab.h ../test/test.h

.PHONY: test test-arm clean
test: $(BUILD)/test_tel
	./$(BUILD)/test_tel
test-arm: $(BUILD)/arm_test_tel
	$(QEMU) ./$(BUILD)/arm_test_tel

$(BUILD)/test_tel: test/test_tel.c $(SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test/test_tel.c $(SRC) -lm
$(BUILD)/arm_test_tel: test/test_tel.c $(SRC) $(HDRS)
	@mkdir -p $(BUILD)
	$(ARM_CC) $(CFLAGS) -static -o $@ test/test_tel.c $(SRC) -lm
clean:
	rm -rf $(BUILD)
