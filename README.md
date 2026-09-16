# PureC-OS-ACPI

ACPI subsystem of PureC-OS: separate kernel module, compiled statically
into the kernel image (same integration pattern as `libxcrypt`).

## What it does

- RSDP / XSDT / RSDT discovery and checksum validation
- FADT parsing: PM1a/PM1b CNT blocks, SMI_CMD + ACPI_ENABLE,
  ResetReg/ResetValue (probed at FADT+110/+116, only a GAS that
  validates is trusted)
- ACPI mode enable (SCI_EN) — required on real laptops, otherwise
  PM1 writes are silently ignored
- DSDT `_S5` parser (minimal, no AML engine): extracts SLP_TYPa/b
  for a spec-correct S5 shutdown
- Shutdown: ACPI S5 (parsed → 7 → 5) → QEMU q35/bochs ports → halt
- Reboot: ResetReg → keyboard controller → CF9 → triple fault
- Read-only MADT walk (LAPIC base, CPU/IOAPIC counts) + table dump
  via `klog` for bare-metal diagnostics
- Battery scan: `BAT0..BAT3`, `PNP0C0A`, AC (`ACPI0003`/`ACAD`/`ADP1`),
  `_BIF`/`_BIX`/`_BST` flags, `SSDT` enumeration, `ECDT`/`PNP0C09`.
  Dynamic values still UNKNOWN (`BATTERY_PERCENT_UNKNOWN`) — no AML executor yet.

## Layout

- `include/acpi/acpi.h` — public API
- `include/acpi/acpi_priv.h` — shared private state (module-internal)
- `src/acpi.c` — init, table walk, GAS access, ACPI enable, dumps
- `src/acpi_s5.c` — `_S5` parser
- `src/acpi_power.c` — shutdown / reboot sequences

## Integration

`src/kernel/Makefile` compiles `acpi/src/*.c` with `KERNEL_CFLAGS`
and `-Iacpi/include -Isrc`. Wiring to bootloader info
(RSDP address + HHDM offset) lives in `src/drivers/power/power.c`
(`power_init()`), the module itself takes them as `acpi_init()` args
and includes no boot globals.

`src/drivers/power/power.c` is a thin wrapper: `power_reboot()`,
`power_shutdown()` and battery reporting delegate to this module.
Syscall numbers are unchanged (`SYS_REBOOT` 218, `SYS_SHUTDOWN` 219).

## Bare-metal notes

- If shutdown still halts instead of powering off, read the serial
  log: `acpi: _S5 parsed ...` vs `SLP_TYP 7/5` fallback lines tell
  whether firmware parsing or the PM1 write failed.
- `ResetReg validated at FADT+...` line tells which FADT layout
  your firmware uses.
- HW-reduced ACPI (no SMI_CMD / no PM blocks, some laptops): the
  module logs it and power ops degrade to legacy fallbacks.
