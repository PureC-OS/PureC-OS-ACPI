CROSS ?= x86_64-elf-
CC := $(CROSS)gcc
LD := $(CROSS)ld

ACPI_DIR := $(abspath $(CURDIR))
KERNEL_SRC ?= $(abspath $(ACPI_DIR)/../src)

CFLAGS := -std=c11 -Wall -Wextra -O2 -ffreestanding -fno-stack-protector \
	-fno-pic -m64 -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
	-I$(ACPI_DIR)/include -I$(KERNEL_SRC) -I$(ACPI_DIR)/AML/uACPI/include

BIN_DIR ?= $(ACPI_DIR)/build
MODULE_DIR := $(BIN_DIR)/modules
BUILD := $(ACPI_DIR)/build/obj

UACPI_SOURCES := $(wildcard $(ACPI_DIR)/AML/uACPI/source/*.c)
UACPI_SOURCES := $(UACPI_SOURCES:$(ACPI_DIR)/%=%)
SOURCES := src/acpi.c src/acpi_s5.c src/acpi_power.c src/uacpi_glue.c src/uacpi_heap.c src/acpi_ec.c src/acpi_battery.c $(UACPI_SOURCES)
MOD_OBJS := $(SOURCES:%.c=$(BUILD)/%.k.o)
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

$(BUILD)/%.k.o: $(ACPI_DIR)/%.c
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
