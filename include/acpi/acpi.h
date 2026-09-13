#pragma once
// PureC-OS ACPI subsystem — public API.
//
// Separate kernel module (own repository), compiled statically into the
// kernel image like libxcrypt. Needs only HHDM offset + RSDP address,
// no other kernel globals: wiring to boot info lives in drivers/power.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// acpi_init: parse RSDP/XSDT/RSDT/FADT, discover DSDT _S5 (SLP_TYP),
// enable ACPI mode via SMI_CMD. Returns 0 on success (tables usable).
// Negative rc = tables unusable, power ops fall back to legacy paths.
//   rsdp_address — value of Limine rsdp_response->address (may be NULL)
//   hhdm_offset  — HHDM offset from Limine (hhdm_offset_global)
int acpi_init(void *rsdp_address, uint64_t hhdm_offset);

// True when FADT was parsed and power ops have real ACPI data.
bool acpi_is_ready(void);

// Log RSDP/XSDT/FADT/DSDT/MADT summary via klog (call after acpi_init).
void acpi_dump_tables(void);

// Find an ACPI table by 4-char signature ("FACP","DSDT","APIC","MCFG"...).
// Returns HHDM-mapped pointer or NULL. Works before/after acpi_init.
void *acpi_find_table(const char *signature);

// Get _S5 sleep types parsed from DSDT AML. Returns true when valid.
bool acpi_get_slp_typ(uint16_t *slp_typa, uint16_t *slp_typb);

// True when DSDT contains a battery device (BAT0).
bool acpi_has_battery(void);

// Power operations. Shutdown tries ACPI S5 -> QEMU ports -> halt.
// Reboot tries FADT ResetReg -> KBC -> CF9 -> triple fault.
// Both normally never return; if they do, the caller must halt.
void acpi_shutdown(void);
void acpi_reboot(void);
