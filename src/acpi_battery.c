#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

#define AML_EXTOP 0x5B
#define AML_DEVICEOP 0x82
#define AML_BYTE_PREFIX 0x0A
#define AML_WORD_PREFIX 0x0B
#define AML_DWORD_PREFIX 0x0C
#define AML_STRING_PREFIX 0x0D
#define AML_RETURNOP 0xA4
#define AML_ZEROOP 0x00
#define AML_ONEOP 0x01

// EISA ID of "PNP0C0A": (('P'-'@')<<26)|(('N'-'@')<<21)|(('P'-'@')<<16)|0x0C0A
#define EISA_PNP0C0A 0x41D00C0Au
// EISA ID of "PNP0C09" (Embedded Controller).
#define EISA_PNP0C09 0x41D00C09u

static struct acpi_battery cached_bat;
static struct acpi_ac_adapter cached_ac;
static bool cached_ec;
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

enum dev_kind {
    DEV_NONE = 0,
    DEV_BATTERY,
    DEV_AC,
    DEV_EC,
};

// Match one Device() body against _HID. body=[bstart,bend).
// On match fills kind/name/uid and returns true.
static bool device_classify(const uint8_t *bstart, const uint8_t *bend,
                            enum dev_kind *kind_out, char name_out[8],
                            uint32_t *uid_out) {
    // Device name is the leading NameSeg; scan the contents after it.
    if (bend - bstart < 4)
        return false;
    for (const uint8_t *q = bstart + 4; q + 4 <= bend; q++) {
        if (memcmp(q, "_HID", 4) != 0)
            continue;
        const uint8_t *r = q + 4;
        if (r >= bend)
            continue;
        enum dev_kind kind = DEV_NONE;
        if (*r == AML_STRING_PREFIX) {
            if (bounded_streq(r + 1, bend, "PNP0C0A"))
                kind = DEV_BATTERY;
            else if (bounded_streq(r + 1, bend, "ACPI0003"))
                kind = DEV_AC;
            else if (bounded_streq(r + 1, bend, "PNP0C09"))
                kind = DEV_EC;
        } else if (*r == AML_DWORD_PREFIX) {
            if (r + 5 <= bend) {
                uint32_t eisa = load_le32(r + 1);
                if (eisa == EISA_PNP0C0A)
                    kind = DEV_BATTERY;
                else if (eisa == EISA_PNP0C09)
                    kind = DEV_EC;
            }
        }
        if (kind == DEV_NONE)
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
        *kind_out = kind;
        return true;
    }
    return false;
}

// Static _STA evaluation: look for a _STA Name/Method whose body is a
// plain Return(Constant). Returns: 1 = present, 0 = absent
// (Return(Zero)), -1 = no static answer (no _STA or dynamic).
static int device_sta_static(const uint8_t *bstart, const uint8_t *bend) {
    for (const uint8_t *q = bstart + 4; q + 4 <= bend; q++) {
        if (memcmp(q, "_STA", 4) != 0)
            continue;
        // Scan a small window after _STA for ReturnOp + constant.
        const uint8_t *win_end = q + 4 + 16;
        if (win_end > bend)
            win_end = bend;
        for (const uint8_t *r = q + 4; r < win_end; r++) {
            if (*r != AML_RETURNOP)
                continue;
            if (r + 1 >= win_end)
                break;
            uint8_t c = r[1];
            if (c == AML_ZEROOP)
                return 0; // Return(Zero): not present
            if (c == AML_ONEOP)
                return 1; // Return(One): present
            if (c == AML_BYTE_PREFIX && r + 2 < win_end)
                return r[2] != 0 ? 1 : 0;
            if (c == AML_WORD_PREFIX && r + 3 < win_end)
                return (r[2] | r[3]) != 0 ? 1 : 0;
            if (c == AML_DWORD_PREFIX && r + 5 < win_end)
                return load_le32(r + 2) != 0 ? 1 : 0;
            break; // dynamic _STA: no static answer
        }
        return -1; // _STA found but not statically evaluable
    }
    return -1; // no _STA: per spec the device is present
}

static int device_psr_static(const uint8_t *bstart, const uint8_t *bend) {
    for (const uint8_t *q = bstart + 4; q + 4 <= bend; q++) {
        if (memcmp(q, "_PSR", 4) != 0)
            continue;
        const uint8_t *win_end = q + 4 + 32;
        if (win_end > bend)
            win_end = bend;
        for (const uint8_t *r = q + 4; r < win_end; r++) {
            if (*r != AML_RETURNOP)
                continue;
            if (r + 1 >= win_end)
                break;
            uint8_t c = r[1];
            if (c == AML_ZEROOP)
                return 0;
            if (c == AML_ONEOP)
                return 1;
            if (c == AML_BYTE_PREFIX && r + 2 < win_end)
                return r[2] != 0 ? 1 : 0;
            if (c == AML_WORD_PREFIX && r + 3 < win_end)
                return (r[2] | r[3]) != 0 ? 1 : 0;
            if (c == AML_DWORD_PREFIX && r + 5 < win_end)
                return load_le32(r + 2) != 0 ? 1 : 0;
            break; // dynamic _PSR: no static answer
        }
        return -1;
    }
    return -1;
}

static void scan_image(const uint8_t *data, const uint8_t *end) {
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
        enum dev_kind kind = DEV_NONE;
        if (!device_classify(bstart, bend, &kind, name, &uid))
            continue;
        int sta = device_sta_static(bstart, bend);
        if (sta == 0) {
            klogf(KLOG_DEBUG, "acpi: device %.4s ignored (_STA Zero)", name);
            continue; // statically absent
        }
        if (kind == DEV_BATTERY && !cached_bat.present) {
            cached_bat.present = true;
            memcpy(cached_bat.name, name, sizeof(cached_bat.name) - 1);
            cached_bat.uid = uid;
            cached_bat.percent_valid = false;
            klogf(KLOG_OK, "acpi: battery device %.4s _HID PNP0C0A _UID %u",
                  name, uid);
        } else if (kind == DEV_AC && !cached_ac.present) {
            cached_ac.present = true;
            memcpy(cached_ac.name, name, sizeof(cached_ac.name) - 1);
            int psr = device_psr_static(bstart, bend);
            if (psr >= 0) {
                cached_ac.online_valid = true;
                cached_ac.online = psr != 0;
                klogf(KLOG_OK, "acpi: AC adapter %.4s static _PSR=%d", name, psr);
            } else {
                cached_ac.online_valid = false;
                klogf(KLOG_OK, "acpi: AC adapter device %.4s _HID ACPI0003", name);
            }
        } else if (kind == DEV_EC && !cached_ec) {
            cached_ec = true;
            klogf(KLOG_INFO, "acpi: embedded controller %.4s present", name);
        }
    }
}

static void scan_table_by_ptr(void *tbl) {
    if (!tbl)
        return;
    struct {
        char signature[4];
        uint32_t length;
    } __attribute__((packed)) *hdr = tbl;
    if (hdr->length < 9 || hdr->length > 16 * 1024 * 1024)
        return;
    const uint8_t *data = (const uint8_t *)tbl;
    const uint8_t *end = data + hdr->length;
    if (end <= data)
        return;
    scan_image(data, end);
}

void acpi_battery_refresh(void) {
    memset(&cached_bat, 0, sizeof(cached_bat));
    memset(&cached_ac, 0, sizeof(cached_ac));
    cached_ec = false;
    cached_valid = false;
    void *dsdt = acpi_find_table("DSDT");
    if (!dsdt) {
        klog(KLOG_WARN, "acpi: no DSDT, battery discovery unavailable");
        cached_valid = true;
        return;
    }
    scan_table_by_ptr(dsdt);
    // Batteries and AC adapters frequently live in SSDTs.
    for (void *ssdt = acpi_next_table("SSDT", NULL); ssdt;
         ssdt = acpi_next_table("SSDT", ssdt))
        scan_table_by_ptr(ssdt);
    if (!cached_bat.present) {
        // Legacy signal: bare "BAT0" reference (method bodies, _BIF users).
        const uint8_t *data = (const uint8_t *)dsdt;
        const uint8_t *end = data + ((uint32_t)data[4] |
                                     ((uint32_t)data[5] << 8) |
                                     ((uint32_t)data[6] << 16) |
                                     ((uint32_t)data[7] << 24));
        for (const uint8_t *p = data; p + 4 <= end; p++) {
            if (memcmp(p, "BAT0", 4) == 0) {
                cached_bat.present = true;
                memcpy(cached_bat.name, "BAT0", 5);
                klog(KLOG_DEBUG, "acpi: battery via BAT0 reference (no _HID device)");
                break;
            }
        }
        if (!cached_bat.present) {
            for (void *ssdt = acpi_next_table("SSDT", NULL); ssdt;
                 ssdt = acpi_next_table("SSDT", ssdt)) {
                const uint8_t *s = (const uint8_t *)ssdt;
                uint32_t len = (uint32_t)s[4] | ((uint32_t)s[5] << 8) |
                               ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 24);
                if (len < 36 || len > 1024 * 1024)
                    continue;
                const uint8_t *se = s + len;
                bool found = false;
                for (const uint8_t *p = s; p + 4 <= se; p++) {
                    if (memcmp(p, "BAT0", 4) == 0) {
                        cached_bat.present = true;
                        memcpy(cached_bat.name, "BAT0", 5);
                        klog(KLOG_DEBUG, "acpi: battery via BAT0 reference in SSDT");
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }
        }
    }
    if (!cached_ac.present) {
        // Fallback: bare ACAD/ADP1 NameSeg reference without _HID.
        const uint8_t *data = (const uint8_t *)dsdt;
        const uint8_t *end = data + ((uint32_t)data[4] |
                                     ((uint32_t)data[5] << 8) |
                                     ((uint32_t)data[6] << 16) |
                                     ((uint32_t)data[7] << 24));
        static const char *ac_names[] = {"ACAD", "ADP1", "ACPI"};
        for (unsigned n = 0; n < 3 && !cached_ac.present; n++) {
            for (const uint8_t *p = data; p + 4 <= end; p++) {
                if (memcmp(p, ac_names[n], 4) == 0) {
                    cached_ac.present = true;
                    memcpy(cached_ac.name,
                           n < 2 ? ac_names[n] : "AC", sizeof(cached_ac.name) - 1);
                    klog(KLOG_DEBUG, "acpi: AC adapter via reference (no _HID device)");
                    break;
                }
            }
        }
    }
    if (!cached_bat.present)
        klog(KLOG_INFO, "acpi: no battery device found");
    if (!cached_ac.present)
        klog(KLOG_INFO, "acpi: no AC adapter device found");
    cached_valid = true;
}

bool acpi_battery_get(struct acpi_battery *out) {
    if (!out)
        return false;
    if (!cached_valid)
        acpi_battery_refresh();
    *out = cached_bat;
    return cached_bat.present;
}

bool acpi_ac_get(struct acpi_ac_adapter *out) {
    if (!out)
        return false;
    if (!cached_valid)
        acpi_battery_refresh();
    *out = cached_ac;
    return cached_ac.present;
}

bool acpi_ec_present(void) {
    if (!cached_valid)
        acpi_battery_refresh();
    return cached_ec;
}

uint32_t acpi_power_source(void) {
    if (!cached_valid)
        acpi_battery_refresh();
    // Without live _PSR/_BST only static facts are known:
    // AC device alone, battery device alone, or nothing (desktop/VM).
    if (cached_ac.present && cached_ac.online_valid)
        return cached_ac.online ? ACPI_POWER_SOURCE_AC : ACPI_POWER_SOURCE_BATTERY;
    if (!cached_bat.present)
        return ACPI_POWER_SOURCE_UNKNOWN; // desktop, VM, QEMU без батареи
    return ACPI_POWER_SOURCE_UNKNOWN; // ноутбук с батареей, но источник неизвестен
}
