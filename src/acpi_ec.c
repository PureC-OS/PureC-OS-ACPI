#include <acpi/acpi.h>

#include "kernel/diagnostics/klog.h"
#include "drivers/interrupts/timer.h"

#include <uacpi/namespace.h>
#include <uacpi/opregion.h>
#include <uacpi/status.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

void *acpi_map_phys(unsigned long long phys);

#define EC_STS_IBF (1u << 0)
#define EC_STS_OBF (1u << 1)

#define EC_CMD_READ 0x80
#define EC_CMD_WRITE 0x81

static uint16_t g_ec_cmd = 0x66;
static uint16_t g_ec_data = 0x62;
static bool g_ec_ports_known = false;
static bool g_ec_installed = false;

static inline uint8_t ec_inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void ec_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(port));
}

static bool ec_wait_ibf_clear(uint32_t timeout_ms) {
    uint64_t start = timer_ticks();
    while (ec_inb(g_ec_cmd) & EC_STS_IBF) {
        if (timer_ticks() - start >= timeout_ms)
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

static bool ec_wait_obf_set(uint32_t timeout_ms) {
    uint64_t start = timer_ticks();
    while (!(ec_inb(g_ec_cmd) & EC_STS_OBF)) {
        if (timer_ticks() - start >= timeout_ms)
            return false;
        __asm__ volatile("pause");
    }
    return true;
}

bool acpi_ec_read(uint8_t offset, uint8_t *out) {
    if (!out)
        return false;
    if (!ec_wait_ibf_clear(100))
        return false;
    ec_outb(g_ec_cmd, EC_CMD_READ);
    if (!ec_wait_ibf_clear(100))
        return false;
    ec_outb(g_ec_data, offset);
    if (!ec_wait_obf_set(100))
        return false;
    *out = ec_inb(g_ec_data);
    return true;
}

bool acpi_ec_write(uint8_t offset, uint8_t value) {
    if (!ec_wait_ibf_clear(100))
        return false;
    ec_outb(g_ec_cmd, EC_CMD_WRITE);
    if (!ec_wait_ibf_clear(100))
        return false;
    ec_outb(g_ec_data, offset);
    if (!ec_wait_ibf_clear(100))
        return false;
    ec_outb(g_ec_data, value);
    if (!ec_wait_ibf_clear(100))
        return false;
    return true;
}

static void ec_parse_ecdt(void) {
    void *ptr = acpi_find_table("ECDT");
    if (!ptr)
        return;
    const uint8_t *t = (const uint8_t *)ptr;
    uint32_t len = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                   ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
    if (len < 60)
        return;
    uint8_t ctl_space = t[36];
    uint64_t ctl_addr = 0;
    for (int i = 7; i >= 0; i--)
        ctl_addr = (ctl_addr << 8) | t[36 + 4 + i];
    uint8_t data_space = t[48];
    uint64_t data_addr = 0;
    for (int i = 7; i >= 0; i--)
        data_addr = (data_addr << 8) | t[48 + 4 + i];
    if (ctl_space == 1 && data_space == 1 && ctl_addr <= 0xFFFF &&
        data_addr <= 0xFFFF && ctl_addr != 0 && data_addr != 0) {
        g_ec_cmd = (uint16_t)ctl_addr;
        g_ec_data = (uint16_t)data_addr;
        g_ec_ports_known = true;
        klogf(KLOG_INFO, "acpi: ECDT ec cmd=0x%x data=0x%x", g_ec_cmd,
              g_ec_data);
    }
}

static uacpi_status ec_region_handler(uacpi_region_op op,
                                      uacpi_handle op_data) {
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE)
        return UACPI_STATUS_OK;
    uacpi_region_rw_data *rw = (uacpi_region_rw_data *)op_data;
    if (!rw || rw->byte_width == 0 || rw->byte_width > 4)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (rw->offset > 0xFF)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (op == UACPI_REGION_OP_READ) {
        uint64_t val = 0;
        for (uint8_t i = 0; i < rw->byte_width; i++) {
            uint8_t b = 0;
            if (!acpi_ec_read((uint8_t)(rw->offset + i), &b))
                return UACPI_STATUS_HARDWARE_TIMEOUT;
            val |= (uint64_t)b << (i * 8);
        }
        rw->value = val;
        return UACPI_STATUS_OK;
    }
    for (uint8_t i = 0; i < rw->byte_width; i++) {
        uint8_t b = (uint8_t)((rw->value >> (i * 8)) & 0xFF);
        if (!acpi_ec_write((uint8_t)(rw->offset + i), b))
            return UACPI_STATUS_HARDWARE_TIMEOUT;
    }
    return UACPI_STATUS_OK;
}

struct ec_find_ctx {
    uacpi_namespace_node *node;
};

static uacpi_iteration_decision ec_find_cb(void *ctx,
                                           uacpi_namespace_node *node,
                                           uacpi_u32 depth) {
    (void)depth;
    struct ec_find_ctx *c = (struct ec_find_ctx *)ctx;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    bool match = hid->value && hid->value[0] == 'P' &&
                 __builtin_strcmp(hid->value, "PNP0C09") == 0;
    uacpi_free_id_string(hid);
    if (match) {
        c->node = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_ec_init(void) {
    ec_parse_ecdt();
    if (!acpi_uacpi_full_ready()) {
        klog(KLOG_DEBUG, "acpi: EC install deferred (uACPI not full yet)");
        return;
    }
    if (g_ec_installed)
        return;
    struct ec_find_ctx ctx = {0};
    uacpi_status st = uacpi_namespace_for_each_child(
        uacpi_namespace_root(), ec_find_cb, NULL, UACPI_OBJECT_DEVICE_BIT,
        UACPI_MAX_DEPTH_ANY, &ctx);
    if (st != UACPI_STATUS_OK || !ctx.node) {
        klog(KLOG_DEBUG, "acpi: no PNP0C09 EC device, EC handler skipped");
        return;
    }
    st = uacpi_install_address_space_handler(
        ctx.node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        ec_region_handler, NULL);
    if (st != UACPI_STATUS_OK) {
        klogf(KLOG_WARN, "acpi: EC handler install failed: %s",
              uacpi_status_to_string(st));
        return;
    }
    st = uacpi_reg_all_opregions(ctx.node,
                                 UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER);
    if (st != UACPI_STATUS_OK)
        klogf(KLOG_DEBUG, "acpi: EC _REG connect returned %s",
              uacpi_status_to_string(st));
    g_ec_installed = true;
    klogf(KLOG_OK, "acpi: EC handler ready (cmd=0x%x data=0x%x)",
          g_ec_cmd, g_ec_data);
}

bool acpi_ec_ready(void) {
    return g_ec_installed;
}
