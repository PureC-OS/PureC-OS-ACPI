#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
int acpi_init(void *rsdp_address, uint64_t hhdm_offset);
bool acpi_is_ready(void);
void acpi_dump_tables(void);
void *acpi_find_table(const char *signature);
typedef void (*acpi_table_visitor)(const char *sig, void *table, uint32_t len, void *ctx);
void acpi_for_each_table(const char *signature, acpi_table_visitor visitor, void *ctx);
#define ACPI_MADT_MAX_CPUS 16
struct acpi_madt_info {
    bool present;
    uint32_t lapic_base;
    uint32_t enabled_cpus;
    uint32_t total_cpus;
    uint32_t ioapic_count;
    uint32_t iso_count;
    uint32_t nmi_count;
    uint32_t lapic_nmi_count;
    uint32_t override_count;
    uint32_t ioapic_first_addr;
    uint8_t lapic_ids[ACPI_MADT_MAX_CPUS];
};
bool acpi_get_madt(struct acpi_madt_info *out);
bool acpi_get_slp_typ(uint16_t *slp_typa, uint16_t *slp_typb);
bool acpi_has_battery(void);
int acpi_battery_count(void);
const char *acpi_battery_name(void);
bool acpi_has_ac(void);
bool acpi_battery_has_bif(void);
bool acpi_battery_has_bst(void);
bool acpi_has_ec(void);
void acpi_shutdown(void);
void acpi_reboot(void);
