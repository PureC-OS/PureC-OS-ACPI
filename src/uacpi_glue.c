#include <acpi/acpi.h>
void *acpi_map_phys(unsigned long long phys);

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/status.h>
#include <uacpi/acpi.h>

static uint64_t g_uacpi_rsdp_phys = 0;

void uacpi_glue_set_rsdp(void *rsdp_virt, uint64_t hhdm) {
    if (!rsdp_virt) {
        g_uacpi_rsdp_phys = 0;
        return;
    }
    uint64_t v = (uint64_t)(uintptr_t)rsdp_virt;
    if (hhdm != 0 && v >= hhdm)
        g_uacpi_rsdp_phys = v - hhdm;
    else
        g_uacpi_rsdp_phys = v;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    if (!out_rsdp_address)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (g_uacpi_rsdp_phys == 0)
        return UACPI_STATUS_NOT_FOUND;
    *out_rsdp_address = g_uacpi_rsdp_phys;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    (void)len;
    if (addr == 0)
        return UACPI_MAP_FAILED;
    return acpi_map_phys((uint64_t)addr);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *text) {
    if (!text)
        return;
    switch (lvl) {
    case UACPI_LOG_DEBUG:
        klog(KLOG_DEBUG, text);
        break;
    case UACPI_LOG_TRACE:
        klog(KLOG_DEBUG, text);
        break;
    case UACPI_LOG_INFO:
        klog(KLOG_INFO, text);
        break;
    case UACPI_LOG_WARN:
        klog(KLOG_WARN, text);
        break;
    case UACPI_LOG_ERROR:
        klog(KLOG_ERROR, text);
        break;
    default:
        klog(KLOG_INFO, text);
        break;
    }
}

static uint8_t g_uacpi_early_table_buf[4096] __attribute__((aligned(8)));
static bool g_uacpi_tables_ready = false;

bool acpi_uacpi_tables_ready(void) {
    return g_uacpi_tables_ready;
}

int acpi_uacpi_early_init(void) {
    if (g_uacpi_rsdp_phys == 0)
        return -1;
    uacpi_status st = uacpi_setup_early_table_access(
        g_uacpi_early_table_buf, sizeof(g_uacpi_early_table_buf));
    if (st != UACPI_STATUS_OK) {
        g_uacpi_tables_ready = false;
        return -2;
    }
    g_uacpi_tables_ready = true;
    klogf(KLOG_OK, "acpi: uACPI early table access ready (tables=%u)",
          (unsigned)uacpi_table_count());
    return 0;
}

bool acpi_uacpi_find_table(const char *sig, void **out_ptr, uint32_t *out_len) {
    if (!sig || !out_ptr)
        return false;
    if (!g_uacpi_tables_ready)
        return false;
    uacpi_table tbl;
    uacpi_status st = uacpi_table_find_by_signature(sig, &tbl);
    if (st != UACPI_STATUS_OK)
        return false;
    struct {
        char signature[4];
        uint32_t length;
    } __attribute__((packed)) *hdr = tbl.ptr;
    if (out_len)
        *out_len = hdr->length;
    *out_ptr = tbl.ptr;
    uacpi_table_unref(&tbl);
    return true;
}

void acpi_uacpi_dump(void) {
    if (!g_uacpi_tables_ready) {
        klog(KLOG_WARN, "acpi: uACPI tables not ready");
        return;
    }
    uacpi_size n = uacpi_table_count();
    klogf(KLOG_INFO, "acpi: uACPI tables=%u", (unsigned)n);
    uacpi_size show = n > 12 ? 12 : n;
    for (uacpi_size i = 0; i < show; i++) {
        uacpi_table tbl;
        if (uacpi_table_get_by_index(i, &tbl) != UACPI_STATUS_OK)
            continue;
        const char *sig = tbl.hdr->signature;
        char s[5];
        s[0] = sig[0];
        s[1] = sig[1];
        s[2] = sig[2];
        s[3] = sig[3];
        s[4] = '\0';
        klogf(KLOG_INFO, "acpi: uACPI [%u] %s len=%u", (unsigned)i, s,
              (unsigned)tbl.hdr->length);
        uacpi_table_unref(&tbl);
    }
}
