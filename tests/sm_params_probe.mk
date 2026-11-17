# Optional read-only hardware probe. Uses an existing open PSL1GHT installation.
.DEFAULT_GOAL := all
ifeq ($(strip $(PSL1GHT)),)
$(error Set PSL1GHT to your existing PSL1GHT installation)
endif
include $(PSL1GHT)/ppu_rules
PROBE_BUILD ?= .local/work/boot/system/probe
.PHONY: all
all: $(PROBE_BUILD)/sm_params_probe.elf
$(PROBE_BUILD)/sm_params_probe.o: tests/sm_params_ps3_probe.c
	mkdir -p $(PROBE_BUILD)
	$(CC) -std=gnu11 -O2 -Wall -mcpu=cell $(MACHDEP) $(LIBPSL1GHT_INC) -c $< -o $@
$(PROBE_BUILD)/sm_params_probe.elf: $(PROBE_BUILD)/sm_params_probe.o
	$(CC) $(MACHDEP) $< $(LIBPSL1GHT_LIB) -lrt -llv2 -o $@
