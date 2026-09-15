#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"

void *g_acpi_rsdp = NULL;
uint64_t g_acpi_hhdm = 0;
struct acpi_fadt_cache g_acpi_fadt;

static bool g_ready = false;
static bool g_battery_present = false;

struct rsdp_v1 {
    char     signature[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_address;
} __attribute__((packed));

struct rsdp_v2 {
    struct rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem[6];
    char     oem_table[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

#define FADT_DSDT         40u
#define FADT_SMI_CMD      48u
#define FADT_ACPI_ENABLE  52u
#define FADT_ACPI_DISABLE 53u
#define FADT_PM1A_EVT     56u
#define FADT_PM1B_EVT     60u
#define FADT_PM1A_CNT     64u
#define FADT_PM1B_CNT     68u
#define FADT_RESET_CANDIDATES_COUNT 2
static const uint32_t fadt_reset_candidates[FADT_RESET_CANDIDATES_COUNT] = {110u, 116u};

void *acpi_map_phys(uint64_t phys) {
    if (phys >= g_acpi_hhdm && g_acpi_hhdm != 0)
        return (void *)(uintptr_t)phys;
    return (void *)(uintptr_t)(phys + g_acpi_hhdm);
}

uint8_t acpi_checksum(const void *p, uint32_t len) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += b[i];
    return sum;
}

bool acpi_table_valid(const void *p, uint32_t len) {
    if (!p || len < sizeof(struct sdt_header))
        return false;
    return acpi_checksum(p, len) == 0;
}

bool acpi_gas_valid(const struct acpi_gas *g) {
    if (!g)
        return false;
    if (g->space != 0 && g->space != 1)
        return false;
    if (g->width != 8 && g->width != 16 && g->width != 32)
        return false;
    if (g->addr == 0)
        return false;
    if (g->space == 1 && g->addr > 0xFFFF)
        return false;
    return true;
}

static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(port));
}

uint16_t acpi_pm_read(uint32_t port) {
    return inw((uint16_t)port);
}

void acpi_pm_write(uint32_t port, uint16_t value) {
    outw((uint16_t)port, value);
}

void acpi_gas_write(const struct acpi_gas *g, uint32_t value) {
    if (!acpi_gas_valid(g))
        return;
    if (g->space == 1) {
        uint16_t port = (uint16_t)g->addr;
        if (g->width <= 8)
            outb(port, (uint8_t)value);
        else if (g->width <= 16)
            outw(port, (uint16_t)value);
        else
            outl(port, value);
        return;
    }
    volatile void *ptr = acpi_map_phys(g->addr);
    if (g->width <= 8)
        *(volatile uint8_t *)ptr = (uint8_t)value;
    else if (g->width <= 16)
        *(volatile uint16_t *)ptr = (uint16_t)value;
    else
        *(volatile uint32_t *)ptr = value;
}

static uint32_t fadt_u32(const uint8_t *f, uint32_t off) {
    return (uint32_t)f[off] | ((uint32_t)f[off + 1] << 8) |
           ((uint32_t)f[off + 2] << 16) | ((uint32_t)f[off + 3] << 24);
}

static uint64_t fadt_u64(const uint8_t *f, uint32_t off) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | f[off + i];
    return v;
}

static void gas_parse(struct acpi_gas *g, const uint8_t *f, uint32_t off) {
    g->space = f[off];
    g->width = f[off + 1];
    g->offset = f[off + 2];
    g->access = f[off + 3];
    g->addr = fadt_u64(f, off + 4);
}

static void acpi_root_tables(struct sdt_header **xsdt, struct sdt_header **rsdt) {
    *xsdt = NULL;
    *rsdt = NULL;
    if (!g_acpi_rsdp)
        return;
    struct rsdp_v1 *r = (struct rsdp_v1 *)g_acpi_rsdp;
    if (r->revision >= 2) {
        struct rsdp_v2 *r2 = (struct rsdp_v2 *)g_acpi_rsdp;
        if (r2->xsdt_address) {
            struct sdt_header *x = (struct sdt_header *)acpi_map_phys(r2->xsdt_address);
            if (x->length >= sizeof(struct sdt_header) && acpi_table_valid(x, x->length))
                *xsdt = x;
        }
    }
    if (r->rsdt_address) {
        struct sdt_header *x = (struct sdt_header *)acpi_map_phys(r->rsdt_address);
        if (x->length >= sizeof(struct sdt_header) && acpi_table_valid(x, x->length))
            *rsdt = x;
    }
}

void *acpi_find_table(const char *signature) {
    if (!signature)
        return NULL;
    struct sdt_header *xsdt = NULL, *rsdt = NULL;
    acpi_root_tables(&xsdt, &rsdt);
    struct sdt_header *roots[2] = {xsdt, rsdt};
    for (int t = 0; t < 2; t++) {
        struct sdt_header *root = roots[t];
        if (!root)
            continue;
        bool wide = (root == xsdt);
        uint32_t stride = wide ? 8u : 4u;
        if (root->length < sizeof(struct sdt_header))
            continue;
        uint32_t entries = (root->length - sizeof(struct sdt_header)) / stride;
        const uint8_t *base = (const uint8_t *)root + sizeof(struct sdt_header);
        for (uint32_t i = 0; i < entries; i++) {
            uint64_t addr;
            if (wide) {
                addr = (uint64_t)base[i * 8] | ((uint64_t)base[i * 8 + 1] << 8) |
                       ((uint64_t)base[i * 8 + 2] << 16) | ((uint64_t)base[i * 8 + 3] << 24) |
                       ((uint64_t)base[i * 8 + 4] << 32) | ((uint64_t)base[i * 8 + 5] << 40) |
                       ((uint64_t)base[i * 8 + 6] << 48) | ((uint64_t)base[i * 8 + 7] << 56);
            } else {
                addr = (uint64_t)base[i * 4] | ((uint64_t)base[i * 4 + 1] << 8) |
                       ((uint64_t)base[i * 4 + 2] << 16) | ((uint64_t)base[i * 4 + 3] << 24);
            }
            if (!addr)
                continue;
            struct sdt_header *tbl = (struct sdt_header *)acpi_map_phys(addr);
            if (tbl->length < sizeof(struct sdt_header))
                continue;
            if (!acpi_table_valid(tbl, tbl->length))
                continue;
            if (memcmp(tbl->signature, signature, 4) == 0)
                return tbl;
        }
    }
    return NULL;
}

static void fadt_parse(struct sdt_header *fadt) {
    memset(&g_acpi_fadt, 0, sizeof(g_acpi_fadt));
    g_acpi_fadt.reset_reg_off = -1;
    if (!fadt)
        return;
    const uint8_t *f = (const uint8_t *)fadt;
    uint32_t len = fadt->length;
    g_acpi_fadt.present = true;
    g_acpi_fadt.rev = fadt->revision;
    if (len > FADT_DSDT)
        g_acpi_fadt.dsdt = fadt_u32(f, FADT_DSDT);
    if (len > FADT_SMI_CMD)
        g_acpi_fadt.smi_cmd = fadt_u32(f, FADT_SMI_CMD);
    if (len > FADT_ACPI_ENABLE)
        g_acpi_fadt.acpi_enable = f[FADT_ACPI_ENABLE];
    if (len > FADT_ACPI_DISABLE)
        g_acpi_fadt.acpi_disable = f[FADT_ACPI_DISABLE];
    if (len > FADT_PM1A_EVT)
        g_acpi_fadt.pm1a_evt = fadt_u32(f, FADT_PM1A_EVT);
    if (len > FADT_PM1B_EVT)
        g_acpi_fadt.pm1b_evt = fadt_u32(f, FADT_PM1B_EVT);
    if (len > FADT_PM1A_CNT) {
        uint32_t p = fadt_u32(f, FADT_PM1A_CNT);
        g_acpi_fadt.pm1a_cnt = (p && p < 0x10000) ? p : 0;
    }
    if (len > FADT_PM1B_CNT) {
        uint32_t p = fadt_u32(f, FADT_PM1B_CNT);
        g_acpi_fadt.pm1b_cnt = (p && p < 0x10000) ? p : 0;
    }
    for (unsigned c = 0; c < FADT_RESET_CANDIDATES_COUNT; c++) {
        uint32_t off = fadt_reset_candidates[c];
        if (len < off + 13)
            continue;
        struct acpi_gas g;
        gas_parse(&g, f, off);
        if (!acpi_gas_valid(&g))
            continue;
        g_acpi_fadt.reset_reg = g;
        g_acpi_fadt.reset_value = f[off + 12];
        g_acpi_fadt.has_reset_reg = true;
        g_acpi_fadt.reset_reg_off = (int)off;
        break;
    }
}

static void delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms * 100000u; i++)
        __asm__ volatile("pause");
}

static void acpi_enable(void) {
    if (!g_acpi_fadt.smi_cmd || !g_acpi_fadt.acpi_enable) {
        klog(KLOG_DEBUG, "acpi: no SMI_CMD/ACPI_ENABLE, assuming HW-reduced or already enabled");
        return;
    }
    if (g_acpi_fadt.pm1a_cnt) {
        uint16_t cnt = acpi_pm_read(g_acpi_fadt.pm1a_cnt);
        if (cnt & 0x0001) {
            klog(KLOG_DEBUG, "acpi: SCI_EN already set, ACPI mode active");
            return;
        }
    }
    klogf(KLOG_INFO, "acpi: enabling ACPI mode via SMI_CMD 0x%x := 0x%x",
          g_acpi_fadt.smi_cmd, g_acpi_fadt.acpi_enable);
    outb((uint16_t)g_acpi_fadt.smi_cmd, g_acpi_fadt.acpi_enable);
    for (int i = 0; i < 300; i++) {
        if (g_acpi_fadt.pm1a_cnt && (acpi_pm_read(g_acpi_fadt.pm1a_cnt) & 0x0001)) {
            klog(KLOG_OK, "acpi: ACPI mode enabled (SCI_EN set)");
            return;
        }
        delay_ms(10);
    }
    klog(KLOG_WARN, "acpi: SCI_EN not observed after ACPI_ENABLE, continuing anyway");
}

static void battery_probe(void) {
    g_battery_present = false;
    struct sdt_header *dsdt = (struct sdt_header *)acpi_find_table("DSDT");
    if (!dsdt)
        return;
    const uint8_t *data = (const uint8_t *)dsdt;
    for (uint32_t i = 0; i + 4 <= dsdt->length; i++) {
        if (memcmp(data + i, "BAT0", 4) == 0) {
            g_battery_present = true;
            klogf(KLOG_DEBUG, "acpi: BAT0 device found in DSDT at offset %u", i);
            return;
        }
    }
}

int acpi_init(void *rsdp_address, uint64_t hhdm_offset) {
    g_acpi_rsdp = rsdp_address;
    g_acpi_hhdm = hhdm_offset;
    g_ready = false;
    memset(&g_acpi_fadt, 0, sizeof(g_acpi_fadt));
    g_acpi_fadt.reset_reg_off = -1;

    if (!g_acpi_rsdp) {
        klog(KLOG_ERROR, "acpi: no RSDP from bootloader, power ops will use legacy fallbacks");
        return -1;
    }
    struct rsdp_v1 *r = (struct rsdp_v1 *)g_acpi_rsdp;
    if (memcmp(r->signature, "RSD PTR ", 8) != 0) {
        klog(KLOG_ERROR, "acpi: RSDP signature mismatch");
        return -2;
    }
    if (acpi_checksum(r, 20) != 0) {
        klog(KLOG_ERROR, "acpi: RSDP v1 checksum mismatch");
        return -2;
    }
    klogf(KLOG_INFO, "acpi: RSDP rev=%u oem=%.6s", r->revision, r->oemid);
    if (r->revision >= 2) {
        struct rsdp_v2 *r2 = (struct rsdp_v2 *)g_acpi_rsdp;
        if (r2->length >= 36 && acpi_checksum(r2, 36) != 0) {
            klog(KLOG_ERROR, "acpi: RSDP v2 checksum mismatch");
            return -2;
        }
    }

    struct sdt_header *fadt = (struct sdt_header *)acpi_find_table("FACP");
    if (!fadt) {
        klog(KLOG_ERROR, "acpi: FADT (FACP) not found, power ops will use legacy fallbacks");
        return -3;
    }
    fadt_parse(fadt);
    klogf(KLOG_INFO, "acpi: FADT rev=%u len=%u dsdt=0x%x pm1a_cnt=0x%x pm1b_cnt=0x%x",
          g_acpi_fadt.rev, fadt->length, g_acpi_fadt.dsdt,
          g_acpi_fadt.pm1a_cnt, g_acpi_fadt.pm1b_cnt);
    if (g_acpi_fadt.has_reset_reg)
        klogf(KLOG_INFO, "acpi: ResetReg validated at FADT+%d space=%u width=%u addr=0x%llx val=0x%x",
              g_acpi_fadt.reset_reg_off, g_acpi_fadt.reset_reg.space,
              g_acpi_fadt.reset_reg.width, (unsigned long long)g_acpi_fadt.reset_reg.addr,
              g_acpi_fadt.reset_value);
    else
        klog(KLOG_WARN, "acpi: no usable ResetReg, reboot will use KBC/CF9/triple-fault");

    acpi_enable();
    acpi_s5_parse();
    battery_probe();

    g_ready = true;
    klog(KLOG_OK, "acpi: subsystem ready");
    return 0;
}

bool acpi_is_ready(void) {
    return g_ready;
}

bool acpi_has_battery(void) {
    return g_battery_present;
}

void acpi_dump_tables(void) {
    if (!g_acpi_rsdp) {
        klog(KLOG_WARN, "acpi: no RSDP, nothing to dump");
        return;
    }
    struct sdt_header *xsdt = NULL, *rsdt = NULL;
    acpi_root_tables(&xsdt, &rsdt);
    klogf(KLOG_INFO, "acpi: root tables xsdt=%p rsdt=%p", xsdt, rsdt);
    struct sdt_header *roots[2] = {xsdt, rsdt};
    const char *names[2] = {"XSDT", "RSDT"};
    for (int t = 0; t < 2; t++) {
        struct sdt_header *root = roots[t];
        if (!root)
            continue;
        bool wide = (root == xsdt);
        uint32_t entries = (root->length - sizeof(struct sdt_header)) / (wide ? 8u : 4u);
        klogf(KLOG_INFO, "acpi: %s entries=%u rev=%u", names[t], entries, root->revision);
        uint32_t show = entries > 12 ? 12 : entries;
        for (uint32_t i = 0; i < show; i++) {
            const uint8_t *base = (const uint8_t *)root + sizeof(struct sdt_header);
            uint64_t addr = 0;
            if (wide) {
                for (int b = 7; b >= 0; b--)
                    addr = (addr << 8) | base[i * 8 + b];
            } else {
                for (int b = 3; b >= 0; b--)
                    addr = (addr << 8) | base[i * 4 + b];
            }
            if (!addr)
                continue;
            struct sdt_header *tbl = (struct sdt_header *)acpi_map_phys(addr);
            if (tbl->length < sizeof(struct sdt_header) || !acpi_table_valid(tbl, tbl->length)) {
                klogf(KLOG_WARN, "acpi:   [%u] invalid table at 0x%llx", i, (unsigned long long)addr);
                continue;
            }
            klogf(KLOG_INFO, "acpi:   [%u] %.4s len=%u rev=%u", i,
                  tbl->signature, tbl->length, tbl->revision);
        }
    }
    struct sdt_header *madt = (struct sdt_header *)acpi_find_table("APIC");
    if (madt && madt->length >= 44) {
        const uint8_t *m = (const uint8_t *)madt;
        uint32_t lapic = (uint32_t)m[36] | ((uint32_t)m[37] << 8) |
                         ((uint32_t)m[38] << 16) | ((uint32_t)m[39] << 24);
        uint32_t counts[6] = {0, 0, 0, 0, 0, 0};
        uint32_t off = 44;
        uint32_t cpus = 0;
        while (off + 2 <= madt->length) {
            uint8_t type = m[off], len = m[off + 1];
            if (len < 2 || off + len > madt->length)
                break;
            if (type < 6)
                counts[type]++;
            if (type == 0 && len >= 8 && (m[off + 4] & 0x01))
                cpus++;
            off += len;
        }
        klogf(KLOG_INFO, "acpi: MADT lapic_base=0x%x enabled_cpus=%u ioapic=%u iso=%u nmi=%u lapic_nmi=%u lapic_override=%u",
              lapic, cpus, counts[1], counts[2], counts[3], counts[4], counts[5]);
    } else {
        klog(KLOG_WARN, "acpi: MADT (APIC) not found");
    }
    if (acpi_find_table("MCFG"))
        klog(KLOG_INFO, "acpi: MCFG present (PCIe ECAM available)");
    uint16_t a = 0, b = 0;
    if (acpi_get_slp_typ(&a, &b))
        klogf(KLOG_INFO, "acpi: _S5 SLP_TYPa=%u SLP_TYPb=%u", a, b);
    else
        klog(KLOG_WARN, "acpi: _S5 not parsed, shutdown will try SLP_TYP 7 then 5");
    klogf(KLOG_INFO, "acpi: battery device: %s", g_battery_present ? "present (BAT0)" : "not found");
}
