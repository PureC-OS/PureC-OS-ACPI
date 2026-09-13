// PureC-OS ACPI power operations: shutdown (S5) and reboot.
// Order matters on bare metal:
//   shutdown: ACPI S5 (parsed _S5, then 7, then 5) -> QEMU ports -> halt
//   reboot:   FADT ResetReg -> KBC pulse -> CF9 -> triple fault
#include <acpi/acpi.h>
#include <acpi/acpi_priv.h>

#include "kernel/diagnostics/klog.h"

#define SLP_EN (1u << 13)
#define SCI_EN (1u << 0)

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

static void delay_ms(uint32_t ms) {
    for (volatile uint32_t i = 0; i < ms * 100000u; i++)
        __asm__ volatile("pause");
}

static void pm_write_s5(uint32_t port, uint16_t slp_typ) {
    if (!port || port > 0xFFFF)
        return;
    uint16_t cur = acpi_pm_read(port);
    // Preserve runtime bits (SCI_EN etc.), replace SLP_TYP, set SLP_EN.
    uint16_t v = (uint16_t)((cur & 0xC1FF) | ((slp_typ & 0x7) << 10) | SLP_EN);
    acpi_pm_write(port, v);
}

void acpi_shutdown(void) {
    klog(KLOG_WARN, "acpi: shutdown requested");

    if (g_acpi_fadt.present && g_acpi_fadt.pm1a_cnt) {
        // Candidate (SLP_TYPa, SLP_TYPb) pairs: parsed _S5 first,
        // then the two values real firmware actually uses.
        uint16_t pairs[3][2];
        int npairs = 0;
        uint16_t a = 0, b = 0;
        if (acpi_get_slp_typ(&a, &b)) {
            pairs[npairs][0] = a;
            pairs[npairs][1] = b;
            npairs++;
        }
        pairs[npairs][0] = 7;
        pairs[npairs][1] = 7;
        npairs++;
        if (a != 5 || b != 5 || npairs == 1) {
            pairs[npairs][0] = 5;
            pairs[npairs][1] = 5;
            npairs++;
        }
        for (int i = 0; i < npairs; i++) {
            klogf(KLOG_INFO, "acpi: trying S5 SLP_TYPa=%u SLP_TYPb=%u on PM1a=0x%x PM1b=0x%x",
                  pairs[i][0], pairs[i][1],
                  g_acpi_fadt.pm1a_cnt, g_acpi_fadt.pm1b_cnt);
            pm_write_s5(g_acpi_fadt.pm1a_cnt, pairs[i][0]);
            if (g_acpi_fadt.pm1b_cnt)
                pm_write_s5(g_acpi_fadt.pm1b_cnt, pairs[i][1]);
            delay_ms(300);
        }
    } else {
        klog(KLOG_WARN, "acpi: no PM1_CNT block, skipping ACPI S5");
    }

    // QEMU/KVM compat: q35 (0x604) and bochs (0xB004) power-off ports.
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outb(0xB2, 0x0F);
    delay_ms(200);

    klog(KLOG_ERROR, "acpi: shutdown failed on this hardware, halting CPU");
    for (;;)
        __asm__ volatile("cli; hlt");
}

void acpi_reboot(void) {
    klog(KLOG_WARN, "acpi: reboot requested");

    if (g_acpi_fadt.has_reset_reg) {
        klogf(KLOG_INFO, "acpi: trying ResetReg (space=%u addr=0x%llx val=0x%x)",
              g_acpi_fadt.reset_reg.space,
              (unsigned long long)g_acpi_fadt.reset_reg.addr,
              g_acpi_fadt.reset_value);
        acpi_gas_write(&g_acpi_fadt.reset_reg, g_acpi_fadt.reset_value);
        delay_ms(500);
    }

    // Keyboard controller reset pulse.
    for (int i = 0; i < 10; i++) {
        if (!(inb(0x64) & 0x02)) {
            outb(0x64, 0xFE);
            delay_ms(100);
        }
    }
    // PCI reset via CF9.
    outb(0xCF9, 0x02);
    delay_ms(100);
    outb(0xCF9, 0x06);
    delay_ms(100);

    klog(KLOG_ERROR, "acpi: reboot fallbacks failed, triple fault");
    __asm__ volatile("cli");
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) bad_idt = {0, 0};
    __asm__ volatile("lidt %0" ::"m"(bad_idt));
    __asm__ volatile("int $0" ::: "memory");
    for (;;)
        __asm__ volatile("cli; hlt");
}
