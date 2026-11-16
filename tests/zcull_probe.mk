# Optional hardware diagnostic; uses an existing open PSL1GHT toolchain only.
.DEFAULT_GOAL := all
ifeq ($(strip $(PSL1GHT)),)
$(error Set PSL1GHT to your existing PSL1GHT installation)
endif
include $(PSL1GHT)/ppu_rules
PROBE_BUILD ?= .local/work/boot/zcull
PROBE_OBJECT := $(PROBE_BUILD)/ps3_probe.o
PROBE_ELF := $(PROBE_BUILD)/ps3_probe.elf
.PHONY: all
all: $(PROBE_ELF)
$(PROBE_OBJECT): tests/zcull_ps3_probe.c tests/zcull_scene.h tests/zcull_cases.h
	mkdir -p $(PROBE_BUILD)
	$(CC) -std=gnu11 -O2 -Wall -mcpu=cell $(MACHDEP) $(LIBPSL1GHT_INC) -Itests -c $< -o $@
$(PROBE_ELF): $(PROBE_OBJECT)
	$(CC) $(MACHDEP) $< $(LIBPSL1GHT_LIB) -lrsx -lgcm_sys -lsysutil -lrt -llv2 -lm -o $@
