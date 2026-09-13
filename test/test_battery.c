// Unit tests for battery discovery (src/acpi_battery.c).
// acpi_find_table() is faked with synthetic DSDT images.
#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t *fake_dsdt;
static uint32_t fake_len;

void *acpi_find_table(const char *signature) {
    if (fake_dsdt && signature && memcmp(signature, "DSDT", 4) == 0) {
        static uint8_t img[1024];
        uint32_t total = fake_len + 8;
        if (total > sizeof(img))
            return NULL;
        memcpy(img, "DSDT", 4);
        img[4] = (uint8_t)total;
        img[5] = (uint8_t)(total >> 8);
        img[6] = (uint8_t)(total >> 16);
        img[7] = (uint8_t)(total >> 24);
        memcpy(img + 8, fake_dsdt, fake_len);
        return img;
    }
    return NULL;
}

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                        \
        }                                                                      \
    } while (0)

// Discovery caches after the first scan (production behavior: refresh
// once at init), so tests re-scan explicitly per case.
static bool get_fresh(struct acpi_battery *out) {
    acpi_battery_refresh();
    return acpi_battery_get(out);
}

// Emit: 5B 82 <pkglen> <NameSeg> <body...>. Returns total bytes.
static uint32_t emit_device(uint8_t *buf, const char *nameseg,
                            const uint8_t *body, uint32_t bodylen) {
    uint8_t *p = buf;
    *p++ = 0x5B;
    *p++ = 0x82;
    uint32_t plen = 4 + bodylen; // NameSeg + body
    if (plen < 64) {
        *p++ = (uint8_t)plen;
    } else {
        *p++ = (uint8_t)(0x40 | (plen & 0x0F));
        *p++ = (uint8_t)(plen >> 4);
    }
    memcpy(p, nameseg, 4);
    p += 4;
    memcpy(p, body, bodylen);
    p += bodylen;
    return (uint32_t)(p - buf);
}

int main(void) {
    uint8_t buf[256];
    struct acpi_battery bat;

    // Device(BAT0) with String _HID + _UID.
    {
        const uint8_t body[] = {
            0x08, '_', 'H', 'I', 'D', 0x0D, 'P', 'N', 'P', '0',
            'C', '0', 'A', 0x00, // Name(_HID, "PNP0C0A")
            0x08, '_', 'U', 'I', 'D', 0x0A, 0x02, // Name(_UID, 2)
        };
        fake_dsdt = buf;
        fake_len = emit_device(buf, "BAT0", body, sizeof(body));
        memset(&bat, 0xAA, sizeof(bat));
        CHECK(get_fresh(&bat));
        CHECK(bat.present && !bat.percent_valid);
        CHECK(memcmp(bat.name, "BAT0", 4) == 0 && bat.uid == 2);
    }
    // EISA DWord _HID form.
    {
        const uint8_t body[] = {
            0x08, '_', 'H', 'I', 'D', 0x0C, 0x0A, 0x0C, 0xD0, 0x41,
        };
        fake_len = emit_device(buf, "BAT1", body, sizeof(body));
        memset(&bat, 0, sizeof(bat));
        CHECK(get_fresh(&bat));
        CHECK(bat.present && memcmp(bat.name, "BAT1", 4) == 0);
    }
    // Long Device body (2-byte PkgLength) with _HID at the end.
    {
        uint8_t big[96];
        memset(big, 0x00, sizeof(big));
        const uint8_t hid[] = {0x08, '_', 'H', 'I', 'D', 0x0D, 'P', 'N',
                               'P', '0', 'C', '0', 'A', 0x00};
        memcpy(big + 60, hid, sizeof(hid));
        fake_dsdt = buf;
        fake_len = emit_device(buf, "BAT0", big, 60 + sizeof(hid));
        memset(&bat, 0, sizeof(bat));
        CHECK(get_fresh(&bat) && bat.present);
    }
    // Wrong _HID: no battery.
    {
        const uint8_t body[] = {
            0x08, '_', 'H', 'I', 'D', 0x0D, 'P', 'N', 'P', '0',
            'C', '0', 'F', 0x00,
        };
        fake_len = emit_device(buf, "EC0_", body, sizeof(body));
        memset(&bat, 0, sizeof(bat));
        CHECK(!get_fresh(&bat) && !bat.present);
    }
    // Bare BAT0 reference without a Device: legacy signal still counts.
    {
        static const uint8_t ref[] = {0x14, 0x10, 'B', 'A', 'T', '0', 0x00};
        fake_dsdt = ref;
        fake_len = sizeof(ref);
        memset(&bat, 0, sizeof(bat));
        CHECK(get_fresh(&bat) && bat.present);
        CHECK(memcmp(bat.name, "BAT0", 4) == 0);
    }
    // Empty DSDT: absent, and discovery must not crash.
    {
        static const uint8_t empty[] = {0x00, 0x00, 0x00, 0x00};
        fake_dsdt = empty;
        fake_len = sizeof(empty);
        memset(&bat, 0, sizeof(bat));
        CHECK(!get_fresh(&bat) && !bat.present);
    }
    // Corrupt DeviceOp (pkglen runs past the end): absent, no crash.
    {
        static const uint8_t corrupt[] = {0x5B, 0x82, 0x7F, 'X', 'Y', 'Z', 'W'};
        fake_dsdt = corrupt;
        fake_len = sizeof(corrupt);
        memset(&bat, 0, sizeof(bat));
        CHECK(!get_fresh(&bat));
    }

    if (!failures)
        printf("test_battery: all passed\n");
    return failures != 0;
}
