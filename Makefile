# PureC-OS ACPI subsystem — standalone build helper.
#
# The real build compiles these sources straight into the kernel image
# (see PureC-OS src/kernel/Makefile, same pattern as libxcrypt):
#   ACPI_SOURCES := acpi/src/acpi.c acpi/src/acpi_s5.c acpi/src/acpi_power.c
# with KERNEL_CFLAGS (-mcmodel=kernel -mgeneral-regs-only) and
# -I$(ACPI_DIR)/include -I$(ROOT_DIR)/src.
#
# This Makefile only syntax-checks the module with the cross compiler.
# KERNEL_SRC must point at the PureC-OS src/ tree (defaults to the
# sibling checkout layout: <root>/acpi + <root>/src).

CROSS ?= x86_64-elf-
CC := $(CROSS)gcc

ACPI_DIR := $(abspath $(CURDIR))
KERNEL_SRC ?= $(abspath $(ACPI_DIR)/../src)

CFLAGS := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-stack-protector \
	-fno-pic -m64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
	-I$(ACPI_DIR)/include -I$(KERNEL_SRC)

SOURCES := src/acpi.c src/acpi_s5.c src/acpi_power.c

.PHONY: all check clean
all: check

check:
	@for s in $(SOURCES); do \
		echo "check $$s"; \
		$(CC) $(CFLAGS) -fsyntax-only $(ACPI_DIR)/$$s || exit 1; \
	done
	@echo "ACPI syntax check OK"

clean:
	@true
