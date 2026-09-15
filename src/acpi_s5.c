#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

static bool s5_valid = false;
static uint16_t s5_a = 0, s5_b = 0;

#define AML_ZERO 0x00
#define AML_ONE 0x01
#define AML_ONES 0xFF
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_DWORD_PREFIX 0x0C
#define AML_PACKAGE_OP 0x12

static uint32_t pkglen_size(const uint8_t *p, const uint8_t *end) {
    if (p >= end)
        return 0;
    uint8_t lead = *p;
    uint32_t follow = (lead >> 6) & 0x03;
    if (p + 1 + follow > end)
        return 0;
    return 1 + follow;
}

static bool parse_aml_int(const uint8_t **pp, const uint8_t *end, uint32_t *out) {
    const uint8_t *p = *pp;
    if (p >= end)
        return false;
    uint8_t op = *p++;
    switch (op) {
    case AML_ZERO:
        *out = 0;
        break;
    case AML_ONE:
        *out = 1;
        break;
    case AML_BYTE_PREFIX:
        if (p >= end)
            return false;
        *out = *p++;
        break;
    case AML_WORD_PREFIX:
        if (p + 2 > end)
            return false;
        *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p += 2;
        break;
    case AML_DWORD_PREFIX:
        if (p + 4 > end)
            return false;
        *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        p += 4;
        break;
    default:
        return false;
    }
    *pp = p;
    return true;
}

static bool try_parse_at(const uint8_t *name, const uint8_t *end) {
    if (name < (const uint8_t *)1 || name[-1] != 0x08)
        return false;
    const uint8_t *p = name + 4;
    uint32_t s = pkglen_size(p, end);
    if (!s)
        return false;
    p += s;
    const uint8_t *scan_end = p + 8 < end ? p + 8 : end;
    while (p < scan_end && *p != AML_PACKAGE_OP)
        p++;
    if (p >= scan_end)
        return false;
    p++;
    s = pkglen_size(p, end);
    if (!s)
        return false;
    p += s;
    if (p >= end)
        return false;
    uint8_t count = *p++;
    if (count < 2)
        return false;
    uint32_t a = 0, b = 0;
    if (!parse_aml_int(&p, end, &a))
        return false;
    if (!parse_aml_int(&p, end, &b))
        return false;
    if (a > 7 || b > 7)
        return false;
    s5_a = (uint16_t)a;
    s5_b = (uint16_t)b;
    return true;
}

void acpi_s5_parse(void) {
    s5_valid = false;
    s5_a = s5_b = 0;
    struct {
        char signature[4];
        uint32_t length;
    } __attribute__((packed)) *dsdt;
    dsdt = acpi_find_table("DSDT");
    if (!dsdt) {
        klog(KLOG_WARN, "acpi: no DSDT, _S5 unavailable");
        return;
    }
    const uint8_t *data = (const uint8_t *)dsdt;
    const uint8_t *end = data + dsdt->length;
    for (const uint8_t *p = data; p + 4 <= end; p++) {
        if (memcmp(p, "_S5_", 4) != 0)
            continue;
        if (try_parse_at(p, end)) {
            s5_valid = true;
            klogf(KLOG_OK, "acpi: _S5 parsed SLP_TYPa=%u SLP_TYPb=%u", s5_a, s5_b);
            return;
        }
        klogf(KLOG_DEBUG, "acpi: _S5_ candidate at +%u rejected",
              (unsigned)(p - data));
    }
    klog(KLOG_WARN, "acpi: _S5_ not found in DSDT, shutdown falls back to SLP_TYP 7/5");
}

bool acpi_get_slp_typ(uint16_t *slp_typa, uint16_t *slp_typb) {
    if (!s5_valid)
        return false;
    if (slp_typa)
        *slp_typa = s5_a;
    if (slp_typb)
        *slp_typb = s5_b;
    return true;
}