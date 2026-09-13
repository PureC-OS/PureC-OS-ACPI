// Unit tests for the DSDT _S5 parser (src/acpi_s5.c).
// acpi_find_table() is faked: it returns a test-controlled DSDT image.
#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const uint8_t *fake_dsdt;
static uint32_t fake_len;

void *acpi_find_table(const char *signature) {
    if (fake_dsdt && signature && memcmp(signature, "DSDT", 4) == 0) {
        // Layout the parser expects: [signature(4)][length(4)][payload].
        static uint8_t img[512];
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

// Build: 08 "_S5_" <pkglen=7> 12 <pkglen=5> 02 <int> <int>
static uint32_t emit_s5(uint8_t *buf, const uint8_t *a, uint32_t alen,
                        const uint8_t *b, uint32_t blen) {
    uint8_t *p = buf;
    *p++ = 0x08;
    memcpy(p, "_S5_", 4);
    p += 4;
    *p++ = (uint8_t)(5 + alen + blen); // PkgLength after NameOp
    *p++ = 0x12;
    *p++ = (uint8_t)(3 + alen + blen); // Package PkgLength
    *p++ = 0x02;                       // two elements
    memcpy(p, a, alen);
    p += alen;
    memcpy(p, b, blen);
    p += blen;
    return (uint32_t)(p - buf);
}

int main(void) {
    uint8_t buf[128];
    uint16_t a = 0, b = 0;

    // BytePrefix pair (3, 4).
    {
        const uint8_t x[] = {0x0A, 0x03};
        const uint8_t y[] = {0x0A, 0x04};
        fake_dsdt = buf;
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(acpi_get_slp_typ(&a, &b) && a == 3 && b == 4);
    }
    // WordPrefix + DWordPrefix pair (7, 5).
    {
        const uint8_t x[] = {0x0B, 0x07, 0x00};
        const uint8_t y[] = {0x0C, 0x05, 0x00, 0x00, 0x00};
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(acpi_get_slp_typ(&a, &b) && a == 7 && b == 5);
    }
    // Zero/One opcodes (0, 1).
    {
        const uint8_t x[] = {0x00};
        const uint8_t y[] = {0x01};
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(acpi_get_slp_typ(&a, &b) && a == 0 && b == 1);
    }
    // No _S5_ at all.
    {
        static const uint8_t no[] = {0x08, 'F', 'O', 'O', '_', 0x01, 0x00};
        fake_dsdt = no;
        fake_len = sizeof(no);
        acpi_s5_parse();
        CHECK(!acpi_get_slp_typ(&a, &b));
    }
    // _S5_ without a preceding NameOp is not a definition.
    {
        static const uint8_t no_op[] = {0x00, '_', 'S', '5', '_', 0x12, 0x01};
        fake_dsdt = no_op;
        fake_len = sizeof(no_op);
        acpi_s5_parse();
        CHECK(!acpi_get_slp_typ(&a, &b));
    }
    // SLP_TYP is 3 bits: 8 and OnesOp are garbage.
    {
        const uint8_t x[] = {0x0A, 0x08};
        const uint8_t y[] = {0x0A, 0x01};
        fake_dsdt = buf;
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(!acpi_get_slp_typ(&a, &b));
    }
    {
        const uint8_t x[] = {0xFF};
        const uint8_t y[] = {0x0A, 0x01};
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(!acpi_get_slp_typ(&a, &b));
    }
    // Truncated package: parser must fail, not over-read.
    {
        const uint8_t x[] = {0x0A, 0x03};
        const uint8_t y[] = {0x0A};
        fake_len = emit_s5(buf, x, sizeof(x), y, sizeof(y));
        acpi_s5_parse();
        CHECK(!acpi_get_slp_typ(&a, &b));
    }

    if (!failures)
        printf("test_s5: all passed\n");
    return failures != 0;
}
