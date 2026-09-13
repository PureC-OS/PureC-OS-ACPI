// PureC-OS ACPI battery discovery.
//
// Finds the Control Method Battery device by walking DSDT Device()
// objects for a _HID of "PNP0C0A" (String form or EISA DWord form),
// records _UID when present. This is real firmware discovery, not a
// string guess — but it only answers "is there a battery".
//
// Live charge percent comes from evaluating _BIF/_BST/_BIX control
// methods, which needs an AML executor (EC OpRegion access). Until
// that exists, percent_valid stays false and callers must treat the
// level as unknown. The struct already carries the field so no API
// break happens when the executor lands.
#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

#define AML_EXTOP 0x5B
#define AML_DEVICEOP 0x82
#define AML_BYTE_PREFIX 0x0A
#define AML_DWORD_PREFIX 0x0C
#define AML_STRING_PREFIX 0x0D

// EISA ID of "PNP0C0A": (('P'-'@')<<26)|(('N'-'@')<<21)|(('P'-'@')<<16)|0x0C0A
#define EISA_PNP0C0A 0x41D00C0Au

static struct acpi_battery cached;
static bool cached_valid = false;

// Decode an AML PkgLength at p (bounded by end). Returns field size,
// stores the length value. 0 = malformed.
static uint32_t pkglen_decode(const uint8_t *p, const uint8_t *end,
                              uint32_t *value) {
    if (p >= end)
        return 0;
    uint8_t lead = *p;
    uint32_t follow = (lead >> 6) & 0x03;
    if (p + 1 + follow > end)
        return 0;
    uint32_t v;
    if (follow == 0) {
        v = lead & 0x3F; // single byte: bits 0-5 are the length
    } else {
        v = lead & 0x0F;
        for (uint32_t i = 0; i < follow; i++)
            v |= (uint32_t)p[1 + i] << (4 + 8 * i);
    }
    if (value)
        *value = v;
    return 1 + follow;
}

// Bounded strcmp inside [s, end): true only for an exact NUL-terminated
// match that fits.
static bool bounded_streq(const uint8_t *s, const uint8_t *end, const char *want) {
    while (*want) {
        if (s >= end || *s != (uint8_t)*want)
            return false;
        s++;
        want++;
    }
    return s < end && *s == '\0';
}

static uint32_t load_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Check one Device() body for _HID PNP0C0A. body=[bstart,bend).
// On match fills name/uid and returns true.
static bool device_is_battery(const uint8_t *bstart, const uint8_t *bend,
                              char name_out[8], uint32_t *uid_out) {
    // Device name is the leading NameSeg; scan the contents after it.
    if (bend - bstart < 4)
        return false;
    for (const uint8_t *q = bstart + 4; q + 4 <= bend; q++) {
        if (memcmp(q, "_HID", 4) != 0)
            continue;
        const uint8_t *r = q + 4;
        if (r >= bend)
            continue;
        bool hid_match = false;
        if (*r == AML_STRING_PREFIX) {
            hid_match = bounded_streq(r + 1, bend, "PNP0C0A");
        } else if (*r == AML_DWORD_PREFIX) {
            hid_match = (r + 5 <= bend && load_le32(r + 1) == EISA_PNP0C0A);
        }
        if (!hid_match)
            continue;
        for (int i = 0; i < 4; i++)
            name_out[i] = (char)bstart[i];
        name_out[4] = '\0';
        *uid_out = 0;
        // Optional _UID Byte right after the NameSeg.
        for (const uint8_t *u = bstart + 4; u + 4 <= bend; u++) {
            if (memcmp(u, "_UID", 4) != 0)
                continue;
            const uint8_t *v = u + 4;
            if (v + 1 < bend && *v == AML_BYTE_PREFIX) {
                *uid_out = v[1];
                break;
            }
        }
        return true;
    }
    return false;
}

void acpi_battery_refresh(void) {
    memset(&cached, 0, sizeof(cached));
    cached_valid = false;
    struct {
        char signature[4];
        uint32_t length;
    } __attribute__((packed)) *dsdt;
    dsdt = acpi_find_table("DSDT");
    if (!dsdt) {
        klog(KLOG_WARN, "acpi: no DSDT, battery discovery unavailable");
        cached_valid = true;
        return;
    }
    const uint8_t *data = (const uint8_t *)dsdt;
    const uint8_t *end = data + dsdt->length;
    for (const uint8_t *p = data; p + 2 <= end; p++) {
        if (p[0] != AML_EXTOP || p[1] != AML_DEVICEOP)
            continue;
        uint32_t plen = 0;
        uint32_t psz = pkglen_decode(p + 2, end, &plen);
        if (!psz || plen < 4)
            continue;
        const uint8_t *bstart = p + 2 + psz;
        if (bstart > end || plen > (uint32_t)(end - (p + 2 + psz)))
            continue;
        const uint8_t *bend = bstart + plen;
        char name[8] = {0};
        uint32_t uid = 0;
        if (device_is_battery(bstart, bend, name, &uid)) {
            cached.present = true;
            memcpy(cached.name, name, sizeof(cached.name) - 1);
            cached.uid = uid;
            cached.percent_valid = false;
            klogf(KLOG_OK, "acpi: battery device %.4s _HID PNP0C0A _UID %u",
                  name, uid);
            break;
        }
    }
    if (!cached.present) {
        // Legacy signal: bare "BAT0" reference (method bodies, _BIF users).
        for (const uint8_t *p = data; p + 4 <= end; p++) {
            if (memcmp(p, "BAT0", 4) == 0) {
                cached.present = true;
                memcpy(cached.name, "BAT0", 5);
                klog(KLOG_DEBUG, "acpi: battery via BAT0 reference (no _HID device)");
                break;
            }
        }
    }
    if (!cached.present)
        klog(KLOG_INFO, "acpi: no battery device found");
    cached_valid = true;
}

bool acpi_battery_get(struct acpi_battery *out) {
    if (!out)
        return false;
    if (!cached_valid)
        acpi_battery_refresh();
    *out = cached;
    return cached.present;
}
