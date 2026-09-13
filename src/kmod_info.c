// Kernel module descriptor: lets the PureC OS module loader
// (src/kernel/module/kmod.c) discover, validate and track this module.
// init/fini are NULL: the kernel's power frontend drives acpi_init()
// explicitly with the bootloader-provided RSDP/HHDM after loading.
#include "kernel/module/kmod_info.h"
#include <stddef.h>

const struct kmod_info kmod_info = {
    KMOD_ABI_VERSION,
    "acpi",
    1,
    NULL,
    NULL,
};
