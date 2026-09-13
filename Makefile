# PureC-OS ACPI subsystem.
#
# Build modes (same pattern as libxcrypt):
#   make            syntax-check sources with the cross compiler
#                   (real build compiles them straight into the kernel
#                   image, see PureC-OS src/kernel/Makefile:
#                   ACPI_SOURCES with KERNEL_CFLAGS and
#                   -I$(ACPI_DIR)/include -I$(ROOT_DIR)/src)
#   make module     relocatable kernel module acpi.elf/.ko
#                   (same pattern as the ext2/crypt modules of PureC OS)
#   make check      syntax-check only
#   make clean      remove build artifacts
#
# When invoked from the PureC OS top-level Makefile, BIN_DIR points at the
# OS bin/ tree so the module lands in bin/modules/. Standalone it defaults
# to the local build/ directory.
# KERNEL_SRC must point at the PureC-OS src/ tree (defaults to the
# sibling checkout layout: <root>/acpi + <root>/src).

CROSS ?= x86_64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld

ACPI_DIR := $(abspath $(CURDIR))
KERNEL_SRC ?= $(abspath $(ACPI_DIR)/../src)

CFLAGS := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-stack-protector \
	-fno-pic -m64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
	-I$(ACPI_DIR)/include -I$(KERNEL_SRC)

BIN_DIR ?= $(ACPI_DIR)/build
MODULE_DIR := $(BIN_DIR)/modules
BUILD := $(ACPI_DIR)/build/obj

SOURCES := src/acpi.c src/acpi_s5.c src/acpi_power.c src/kmod_info.c
MOD_OBJS := $(SOURCES:src/%.c=$(BUILD)/%.k.o)
MOD_ELF := $(MODULE_DIR)/acpi.elf
MOD_KO := $(MODULE_DIR)/acpi.ko

.PHONY: all check module clean
all: check

check:
	@for s in $(SOURCES); do \
		echo "check $$s"; \
		$(CC) $(CFLAGS) -fsyntax-only $(ACPI_DIR)/$$s || exit 1; \
	done
	@echo "ACPI syntax check OK"

module: $(MOD_ELF) $(MOD_KO)

$(BUILD)/%.k.o: $(ACPI_DIR)/src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(MOD_ELF): $(MOD_OBJS)
	@mkdir -p $(@D)
	$(LD) -r -o $@ $^
	@echo "acpi module -> $@"

$(MOD_KO): $(MOD_ELF)
	@cp $< $@
	@echo "acpi module -> $@"

-include $(MOD_OBJS:.o=.d)

clean:
	@rm -rf $(ACPI_DIR)/build
