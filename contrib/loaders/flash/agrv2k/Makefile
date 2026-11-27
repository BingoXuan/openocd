# SPDX-License-Identifier: GPL-2.0-or-later
CROSS_COMPILE ?= riscv64-unknown-elf-
CC = $(CROSS_COMPILE)gcc
OBJCOPY = $(CROSS_COMPILE)objcopy
BIN2C = ../../../../src/helper/bin2char.sh

all: agrv2k.inc

agrv2k.elf: agrv2k.S loader.ld
	$(CC) -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -Wl,-T,loader.ld -Wl,--no-relax $< -o $@

agrv2k.bin: agrv2k.elf
	$(OBJCOPY) -O binary -j .text $< $@

agrv2k.inc: agrv2k.bin
	$(BIN2C) < $< > $@

.PHONY: clean
clean:
	rm -f agrv2k.elf agrv2k.bin
