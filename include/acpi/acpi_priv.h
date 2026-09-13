#pragma once
// Private shared state of the ACPI module (not a public API).
// Included by acpi/src/*.c only.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Generic Address Structure (ACPI 6.x, 12 bytes).
struct acpi_gas {
    uint8_t  space;  // 0 = SystemMemory, 1 = SystemIO
    uint8_t  width;  // register bit width: 8/16/32
    uint8_t  offset; // bit offset
    uint8_t  access; // access size
    uint64_t addr;
};

// Cached FADT power-management registers.
struct acpi_fadt_cache {
    bool     present;
    uint8_t  rev;
    uint32_t dsdt;        // legacy DSDT physical address (FADT+40)
    uint32_t smi_cmd;     // FADT+48 (0 = no legacy SMI switch)
    uint8_t  acpi_enable; // FADT+52
    uint8_t  acpi_disable;// FADT+53
    uint32_t pm1a_evt;    // FADT+56
    uint32_t pm1b_evt;    // FADT+60
    uint32_t pm1a_cnt;    // FADT+64 legacy I/O port (0 = unknown)
    uint32_t pm1b_cnt;    // FADT+68 legacy I/O port (0 = unknown/absent)
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    bool     has_reset_reg;
    int      reset_reg_off; // FADT offset the GAS validated at (110/116/-1)
};

extern struct acpi_fadt_cache g_acpi_fadt;
extern void *g_acpi_rsdp;
extern uint64_t g_acpi_hhdm;

// Physical -> HHDM-mapped pointer.
void *acpi_map_phys(uint64_t phys);

uint8_t acpi_checksum(const void *p, uint32_t len);
bool acpi_table_valid(const void *p, uint32_t len);

// Strict GAS validation: only IO/Memory, sane width, non-zero address.
bool acpi_gas_valid(const struct acpi_gas *g);

// GAS write of an 8/16/32-bit value (IO port or MMIO via HHDM).
void acpi_gas_write(const struct acpi_gas *g, uint32_t value);

// Legacy PM1_CNT I/O helpers (port validated by caller).
uint16_t acpi_pm_read(uint32_t port);
void acpi_pm_write(uint32_t port, uint16_t value);

// Parse DSDT _S5 package into SLP_TYPa/b. Called once from acpi_init.
void acpi_s5_parse(void);

// Discover the battery device in DSDT (_HID PNP0C0A) and cache it.
// Called once from acpi_init.
void acpi_battery_refresh(void);
