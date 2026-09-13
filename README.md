# PureC-OS-ACPI

ACPI subsystem of PureC-OS: separate kernel module, loaded at boot
by the kernel module loader (`src/kernel/module/kmod.c` in the main
repo) from `/bin/modules/acpi.elf`.

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
- Battery device discovery: walks DSDT `Device()` objects for
  `_HID "PNP0C0A"` (String or EISA DWord form), records `_UID`.
  Charge percent is honestly reported as unknown (`percent_valid =
  false`): live `_BIF`/`_BST` evaluation needs an AML method executor,
  which is roadmap, not this module. The API already carries the
  field so no break happens later.

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

`src/drivers/power/power.c` is a thin wrapper: it loads the module
via `kmod`, resolves `acpi_*` symbols and falls back to legacy paths
when the module is absent. Syscall numbers are unchanged
(`SYS_REBOOT` 218, `SYS_SHUTDOWN` 219).

## Testing and CI

- `make test` — host unit tests (needs system `cc`): `test_s5`
  (`_S5` variants, garbage, truncation), `test_tables` (full init
  path on synthetic RSDP/XSDT/FADT/DSDT, checksum failures,
  ResetReg probe, GAS matrix), `test_battery` (String/EISA `_HID`,
  `_UID`, corrupt input). Pure logic only; `acpi_power.c` is
  excluded (port I/O, bare metal only).
- `make check` / `make module` — freestanding cross build.
- `.github/workflows/build-and-test.yml` — test → build → ABI guard
  (undefined module symbols must be in the kernel `ksymtab`
  allowlist, `kmod_info` must be exported) → release archives on tags.

## Bare-metal notes

- If shutdown still halts instead of powering off, read the serial
  log: `acpi: _S5 parsed ...` vs `SLP_TYP 7/5` fallback lines tell
  whether firmware parsing or the PM1 write failed.
- `ResetReg validated at FADT+...` line tells which FADT layout
  your firmware uses.
- HW-reduced ACPI (no SMI_CMD / no PM blocks, some laptops): the
  module logs it and power ops degrade to legacy fallbacks.
