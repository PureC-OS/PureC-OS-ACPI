# PureC-OS ACPI subsystem.
#
# Build modes (same pattern as libxcrypt):
#   make            syntax-check sources with the cross compiler
#                   (the PureC OS kernel loads the module at boot via
#                   its module loader, see src/kernel/module/kmod.c)
#   make module     relocatable kernel module acpi.elf/.ko
#                   (same pattern as the ext2/crypt modules of PureC OS)
#   make check      syntax-check only
#   make test       host-compile + run unit tests (needs system cc)
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

SOURCES := src/acpi.c src/acpi_s5.c src/acpi_power.c src/acpi_battery.c src/kmod_info.c
MOD_OBJS := $(SOURCES:src/%.c=$(BUILD)/%.k.o)
MOD_ELF := $(MODULE_DIR)/acpi.elf
MOD_KO := $(MODULE_DIR)/acpi.ko

# Host unit tests: pure logic (tables, _S5, battery discovery) builds
# against stub kernel headers. acpi_power.c is excluded on purpose:
# it executes port I/O and needs bare metal.
HOST_CC ?= gcc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 \
	-I$(ACPI_DIR)/include -I$(ACPI_DIR)/test/stubs
TEST_BIN := $(ACPI_DIR)/build/test
TEST_S5 := $(TEST_BIN)/test_s5
TEST_TABLES := $(TEST_BIN)/test_tables
TEST_BATTERY := $(TEST_BIN)/test_battery

.PHONY: all check module test clean
all: check

test: $(TEST_S5) $(TEST_TABLES) $(TEST_BATTERY)
	$(TEST_S5)
	$(TEST_TABLES)
	$(TEST_BATTERY)

$(TEST_S5): $(ACPI_DIR)/test/test_s5.c $(ACPI_DIR)/src/acpi_s5.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

$(TEST_TABLES): $(ACPI_DIR)/test/test_tables.c $(ACPI_DIR)/src/acpi.c $(ACPI_DIR)/src/acpi_s5.c $(ACPI_DIR)/src/acpi_battery.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

$(TEST_BATTERY): $(ACPI_DIR)/test/test_battery.c $(ACPI_DIR)/src/acpi_battery.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $^ -o $@

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
