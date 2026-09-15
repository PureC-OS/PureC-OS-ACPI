#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
int acpi_init(void *rsdp_address, uint64_t hhdm_offset);
bool acpi_is_ready(void);
void acpi_dump_tables(void);
void *acpi_find_table(const char *signature);
bool acpi_get_slp_typ(uint16_t *slp_typa, uint16_t *slp_typb);
bool acpi_has_battery(void);
void acpi_shutdown(void);
void acpi_reboot(void);
