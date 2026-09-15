#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

struct acpi_gas {
    uint8_t  space;
    uint8_t  width;
    uint8_t  offset;
    uint8_t  access;
    uint64_t addr;
};

struct acpi_fadt_cache {
    bool     present;
    uint8_t  rev;
    uint32_t dsdt;
    uint32_t smi_cmd;
    uint8_t  acpi_enable;
    uint8_t  acpi_disable;
    uint32_t pm1a_evt;
    uint32_t pm1b_evt;
    uint32_t pm1a_cnt;
    uint32_t pm1b_cnt;
    struct acpi_gas reset_reg;
    uint8_t  reset_value;
    bool     has_reset_reg;
    int      reset_reg_off;
};

extern struct acpi_fadt_cache g_acpi_fadt;
extern void *g_acpi_rsdp;
extern uint64_t g_acpi_hhdm;
void *acpi_map_phys(uint64_t phys);
uint8_t acpi_checksum(const void *p, uint32_t len);
bool acpi_table_valid(const void *p, uint32_t len);
bool acpi_gas_valid(const struct acpi_gas *g);
void acpi_gas_write(const struct acpi_gas *g, uint32_t value);
uint16_t acpi_pm_read(uint32_t port);
void acpi_pm_write(uint32_t port, uint16_t value);
void acpi_s5_parse(void);