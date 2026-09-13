// Integration test for the ACPI init path (real src/acpi.c +
// acpi_s5.c + acpi_battery.c) on synthetic in-memory tables.
// Pointers double as "physical" addresses (hhdm = 0 in this test).
#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                        \
        }                                                                      \
    } while (0)

struct fake_rsdp {
    char sig[8];
    uint8_t csum;
    char oem[6];
    uint8_t rev;
    uint32_t rsdt;
    uint32_t len;
    uint64_t xsdt;
    uint8_t xcsum;
    uint8_t rsv[3];
} __attribute__((packed));

struct fake_sdt {
    char sig[4];
    uint32_t len;
    uint8_t rev;
    uint8_t csum;
    uint8_t rest[28];
} __attribute__((packed));

static void fix_checksum(uint8_t *base, uint32_t total, uint32_t csum_off) {
    base[csum_off] = 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < total; i++)
        sum += base[i];
    base[csum_off] = (uint8_t)((256 - (sum % 256)) % 256);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint8_t xsdt_img[128];
static uint8_t facp_img[256];
static uint8_t dsdt_img[512];
static struct fake_rsdp rsdp_img;

static void build_tables(void) {
    memset(xsdt_img, 0, sizeof(xsdt_img));
    memset(facp_img, 0, sizeof(facp_img));
    memset(dsdt_img, 0, sizeof(dsdt_img));
    memset(&rsdp_img, 0, sizeof(rsdp_img));

    // DSDT: _S5_ (Byte 7, 7) + Device(BAT0) with String _HID.
    uint8_t *d = dsdt_img;
    memcpy(d, "DSDT", 4);
    put_u32(d + 4, 36 + 64); // length
    d[8] = 2;
    uint8_t *p = d + 36;
    *p++ = 0x08;
    memcpy(p, "_S5_", 4);
    p += 4;
    *p++ = 0x09; // PkgLength
    *p++ = 0x12;
    *p++ = 0x07; // PkgLength
    *p++ = 0x02;
    *p++ = 0x0A;
    *p++ = 0x07;
    *p++ = 0x0A;
    *p++ = 0x07;
    *p++ = 0x5B; // DeviceOp
    *p++ = 0x82;
    *p++ = 18; // PkgLength: NameSeg + 14 body bytes
    memcpy(p, "BAT0", 4);
    p += 4;
    *p++ = 0x08; // NameOp
    memcpy(p, "_HID", 4);
    p += 4;
    *p++ = 0x0D; // String
    memcpy(p, "PNP0C0A", 8);
    p += 8;
    *p++ = 0x00;
    fix_checksum(d, 36 + 64, 9);

    // FACP: rev 5, no SMI (host has no I/O), garbage GAS at 110,
    // valid IO ResetReg at 116 (tests the probe fallback).
    uint8_t *f = facp_img;
    memcpy(f, "FACP", 4);
    put_u32(f + 4, 136);
    f[8] = 5;
    f[110] = 9; // invalid space -> probe must skip +110
    f[116] = 1; // space: SystemIO
    f[117] = 8; // width
    f[118] = 0;
    f[119] = 0;
    put_u64(f + 120, 0xCF9);
    f[128] = 0x06; // reset value
    fix_checksum(f, 136, 9);

    // XSDT with FACP + DSDT entries.
    uint8_t *x = xsdt_img;
    memcpy(x, "XSDT", 4);
    put_u32(x + 4, 36 + 16);
    x[8] = 1;
    put_u64(x + 36, (uint64_t)(uintptr_t)facp_img);
    put_u64(x + 44, (uint64_t)(uintptr_t)dsdt_img);
    fix_checksum(x, 36 + 16, 9);

    // RSDP v2 pointing at the XSDT.
    memcpy(rsdp_img.sig, "RSD PTR ", 8);
    memcpy(rsdp_img.oem, "PUREOS", 6);
    rsdp_img.rev = 2;
    rsdp_img.len = 36;
    rsdp_img.xsdt = (uint64_t)(uintptr_t)xsdt_img;
    fix_checksum((uint8_t *)&rsdp_img, 20, 8);
    fix_checksum((uint8_t *)&rsdp_img, 36, 32);
}

int main(void) {
    build_tables();

    CHECK(acpi_init(&rsdp_img, 0) == 0);
    CHECK(acpi_is_ready());
    CHECK(acpi_find_table("FACP") == facp_img);
    CHECK(acpi_find_table("DSDT") == dsdt_img);
    CHECK(acpi_find_table("APIC") == NULL);
    CHECK(acpi_find_table(NULL) == NULL);

    uint16_t a = 0, b = 0;
    CHECK(acpi_get_slp_typ(&a, &b) && a == 7 && b == 7);
    CHECK(acpi_has_battery());

    struct acpi_battery bat;
    memset(&bat, 0, sizeof(bat));
    CHECK(acpi_battery_get(&bat) && bat.present && !bat.percent_valid);
    CHECK(memcmp(bat.name, "BAT0", 4) == 0);

    // FADT probe must have skipped the bogus +110 GAS for +116.
    CHECK(g_acpi_fadt.has_reset_reg);
    CHECK(g_acpi_fadt.reset_reg_off == 116);
    CHECK(g_acpi_fadt.reset_reg.space == 1);
    CHECK(g_acpi_fadt.reset_reg.addr == 0xCF9);
    CHECK(g_acpi_fadt.reset_value == 0x06);

    // GAS validation matrix.
    {
        struct acpi_gas g;
        CHECK(!acpi_gas_valid(NULL));
        memset(&g, 0, sizeof(g));
        CHECK(!acpi_gas_valid(&g)); // zero address
        g.space = 9;
        g.width = 8;
        g.addr = 0xCF9;
        CHECK(!acpi_gas_valid(&g)); // bad space
        g.space = 1;
        g.width = 64;
        CHECK(!acpi_gas_valid(&g)); // bad width
        g.width = 16;
        g.addr = 0x10000;
        CHECK(!acpi_gas_valid(&g)); // IO out of range
        g.addr = 0x3F8;
        CHECK(acpi_gas_valid(&g));
        g.space = 0;
        g.width = 32;
        g.addr = 0xFED00000;
        CHECK(acpi_gas_valid(&g));
    }

    acpi_dump_tables(); // must not crash on the synthetic chain

    // Corrupt the XSDT checksum: lookups must fail closed.
    xsdt_img[9] ^= 0xFF;
    CHECK(acpi_find_table("FACP") == NULL);
    xsdt_img[9] ^= 0xFF;

    // Bad inputs to init.
    CHECK(acpi_init(NULL, 0) == -1);
    {
        struct fake_rsdp bad = rsdp_img;
        memcpy(bad.sig, "NOPE!!!!", 8);
        CHECK(acpi_init(&bad, 0) == -2);
    }

    if (!failures)
        printf("test_tables: all passed\n");
    return failures != 0;
}
