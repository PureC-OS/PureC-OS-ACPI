#include <acpi/acpi.h>
void *acpi_map_phys(unsigned long long phys);

#include "kernel/diagnostics/klog.h"
#include "lib/string.h"
#include "drivers/interrupts/timer.h"
#include "kernel/process/scheduler.h"
#include "drivers/pci/pci.h"
#include "arch/x86_64/idt/include/idt.h"

#include <uacpi/kernel_api.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/status.h>
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>

void *uheap_alloc(uint64_t size);
void *uheap_alloc_zeroed(uint64_t size);
void uheap_free(void *ptr);

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

void *uacpi_kernel_alloc(uacpi_size size) {
    return uheap_alloc((uint64_t)size);
}

void *uacpi_kernel_alloc_zeroed(uacpi_size size) {
    return uheap_alloc_zeroed((uint64_t)size);
}

void uacpi_kernel_free(void *mem) {
    uheap_free(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return (uacpi_u64)timer_ticks() * 1000000ULL;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    for (volatile uint32_t i = 0; i < (uint32_t)usec * 100u; i++)
        __asm__ volatile("pause");
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    if (msec > 0xFFFFFFFFULL)
        msec = 0xFFFFFFFFULL;
    timer_sleep((uint32_t)msec);
}

struct uacpi_mutex {
    volatile int locked;
};

uacpi_handle uacpi_kernel_create_mutex(void) {
    struct uacpi_mutex *m = uheap_alloc(sizeof(*m));
    if (!m)
        return NULL;
    m->locked = 0;
    return (uacpi_handle)m;
}

void uacpi_kernel_free_mutex(uacpi_handle h) {
    uheap_free(h);
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout) {
    struct uacpi_mutex *m = (struct uacpi_mutex *)h;
    if (!m)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (timeout == 0) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&m->locked, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return UACPI_STATUS_OK;
        return UACPI_STATUS_TIMEOUT;
    }
    uint64_t start = timer_ticks();
    uint64_t limit = (timeout == 0xFFFF) ? UINT64_MAX : (uint64_t)timeout;
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&m->locked, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return UACPI_STATUS_OK;
        if (timeout != 0xFFFF && timer_ticks() - start >= limit)
            return UACPI_STATUS_TIMEOUT;
        scheduler_yield();
        __asm__ volatile("pause");
    }
}

void uacpi_kernel_release_mutex(uacpi_handle h) {
    struct uacpi_mutex *m = (struct uacpi_mutex *)h;
    if (!m)
        return;
    __atomic_store_n(&m->locked, 0, __ATOMIC_RELEASE);
}

struct uacpi_event {
    volatile uint64_t count;
};

uacpi_handle uacpi_kernel_create_event(void) {
    struct uacpi_event *e = uheap_alloc_zeroed(sizeof(*e));
    return (uacpi_handle)e;
}

void uacpi_kernel_free_event(uacpi_handle h) {
    uheap_free(h);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout) {
    struct uacpi_event *e = (struct uacpi_event *)h;
    if (!e)
        return UACPI_FALSE;
    uint64_t start = timer_ticks();
    uint64_t limit = (timeout == 0xFFFF) ? UINT64_MAX : (uint64_t)timeout;
    for (;;) {
        uint64_t c = __atomic_load_n(&e->count, __ATOMIC_ACQUIRE);
        if (c > 0) {
            uint64_t exp = c;
            if (__atomic_compare_exchange_n(&e->count, &exp, c - 1, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
                return UACPI_TRUE;
            continue;
        }
        if (timeout != 0xFFFF && timer_ticks() - start >= limit)
            return UACPI_FALSE;
        scheduler_yield();
        __asm__ volatile("pause");
    }
}

void uacpi_kernel_signal_event(uacpi_handle h) {
    struct uacpi_event *e = (struct uacpi_event *)h;
    if (!e)
        return;
    __atomic_add_fetch(&e->count, 1, __ATOMIC_RELEASE);
}

void uacpi_kernel_reset_event(uacpi_handle h) {
    struct uacpi_event *e = (struct uacpi_event *)h;
    if (!e)
        return;
    __atomic_store_n(&e->count, 0, __ATOMIC_RELEASE);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    int tid = scheduler_current_tid();
    if (tid < 0)
        return (uacpi_thread_id)(uintptr_t)1;
    return (uacpi_thread_id)(uintptr_t)(tid + 1);
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return (uacpi_interrupt_state)f;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state & (1UL << 9))
        __asm__ volatile("sti" ::: "memory");
}

struct uacpi_spinlock {
    volatile int locked;
};

uacpi_handle uacpi_kernel_create_spinlock(void) {
    struct uacpi_spinlock *s = uheap_alloc(sizeof(*s));
    if (!s)
        return NULL;
    s->locked = 0;
    return (uacpi_handle)s;
}

void uacpi_kernel_free_spinlock(uacpi_handle h) {
    uheap_free(h);
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    struct uacpi_spinlock *s = (struct uacpi_spinlock *)h;
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    if (s) {
        while (__atomic_test_and_set(&s->locked, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }
    return (uacpi_cpu_flags)f;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags) {
    struct uacpi_spinlock *s = (struct uacpi_spinlock *)h;
    if (s)
        __atomic_clear(&s->locked, __ATOMIC_RELEASE);
    if (flags & (1UL << 9))
        __asm__ volatile("sti" ::: "memory");
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    if (!req)
        return UACPI_STATUS_INVALID_ARGUMENT;
    klogf(KLOG_WARN, "acpi: firmware request type=%d", (int)req->type);
    return UACPI_STATUS_OK;
}

struct uacpi_irq_handle {
    uint32_t irq;
    uacpi_interrupt_handler handler;
    uacpi_handle ctx;
};

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle) {
    if (!handler || !out_irq_handle)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (irq >= 16) {
        klogf(KLOG_WARN, "acpi: SCI irq %u needs IOAPIC, not supported",
              (unsigned)irq);
        return UACPI_STATUS_UNIMPLEMENTED;
    }
    struct uacpi_irq_handle *h = uheap_alloc(sizeof(*h));
    if (!h)
        return UACPI_STATUS_OUT_OF_MEMORY;
    h->irq = irq;
    h->handler = handler;
    h->ctx = ctx;
    idt_set_irq_handler((uint8_t)irq, (void *)handler, ctx);
    idt_unmask_irq((uint8_t)irq);
    *out_irq_handle = (uacpi_handle)h;
    klogf(KLOG_OK, "acpi: IRQ %u handler installed", (unsigned)irq);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler handler, uacpi_handle irq_handle) {
    struct uacpi_irq_handle *h = (struct uacpi_irq_handle *)irq_handle;
    if (!h)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (h->irq < 16) {
        idt_mask_irq((uint8_t)h->irq);
        idt_clear_irq_handler((uint8_t)h->irq);
    }
    (void)handler;
    uheap_free(h);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_schedule_work(
    uacpi_work_type type, uacpi_work_handler handler, uacpi_handle ctx) {
    (void)type;
    if (!handler)
        return UACPI_STATUS_INVALID_ARGUMENT;
    handler(ctx);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    return UACPI_STATUS_OK;
}

struct uacpi_pci_handle {
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
};

uacpi_status uacpi_kernel_pci_device_open(
    uacpi_pci_address address, uacpi_handle *out_handle) {
    if (!out_handle)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (address.device > 31 || address.function > 7)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t id = pci_read_config32((uint8_t)address.bus,
                                    (uint8_t)address.device,
                                    (uint8_t)address.function, 0x00);
    if ((uint16_t)id == 0xFFFF)
        return UACPI_STATUS_NOT_FOUND;
    struct uacpi_pci_handle *h = uheap_alloc(sizeof(*h));
    if (!h)
        return UACPI_STATUS_OUT_OF_MEMORY;
    h->bus = (uint8_t)address.bus;
    h->dev = (uint8_t)address.device;
    h->func = (uint8_t)address.function;
    *out_handle = (uacpi_handle)h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle h) {
    uheap_free(h);
}

static struct uacpi_pci_handle *pci_h(uacpi_handle h) {
    return (struct uacpi_pci_handle *)h;
}

uacpi_status uacpi_kernel_pci_read8(
    uacpi_handle dev, uacpi_size off, uacpi_u8 *v) {
    if (!dev || !v || off > 255)
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    uint32_t w = pci_read_config32(h->bus, h->dev, h->func, (uint8_t)(off & ~3u));
    *v = (uacpi_u8)((w >> ((off & 3) * 8)) & 0xFF);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(
    uacpi_handle dev, uacpi_size off, uacpi_u16 *v) {
    if (!dev || !v || off > 254 || (off & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    uint32_t w = pci_read_config32(h->bus, h->dev, h->func, (uint8_t)(off & ~3u));
    *v = (uacpi_u16)((w >> ((off & 3) * 8)) & 0xFFFF);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(
    uacpi_handle dev, uacpi_size off, uacpi_u32 *v) {
    if (!dev || !v || off > 252 || (off & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    *v = pci_read_config32(h->bus, h->dev, h->func, (uint8_t)off);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(
    uacpi_handle dev, uacpi_size off, uacpi_u8 v) {
    if (!dev || off > 255)
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    pci_write_config8(h->bus,h->dev,h->func,(uint8_t)off,v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(
    uacpi_handle dev, uacpi_size off, uacpi_u16 v) {
    if (!dev || off > 254 || (off & 1))
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    pci_write_config16(h->bus,h->dev,h->func,(uint8_t)off,v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(
    uacpi_handle dev, uacpi_size off, uacpi_u32 v) {
    if (!dev || off > 252 || (off & 3))
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_pci_handle *h = pci_h(dev);
    pci_write_config32(h->bus, h->dev, h->func, (uint8_t)off, v);
    return UACPI_STATUS_OK;
}

struct uacpi_io_handle {
    uint16_t base;
    uint64_t len;
};

static inline uint8_t io_inb(uint16_t p) {
    uint8_t v;
    __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint16_t io_inw(uint16_t p) {
    uint16_t v;
    __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline uint32_t io_inl(uint16_t p) {
    uint32_t v;
    __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void io_outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p));
}
static inline void io_outw(uint16_t p, uint16_t v) {
    __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p));
}
static inline void io_outl(uint16_t p, uint32_t v) {
    __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p));
}

uacpi_status uacpi_kernel_io_map(
    uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    if (!out_handle || len == 0 || base > 0xFFFF)
        return UACPI_STATUS_INVALID_ARGUMENT;
    struct uacpi_io_handle *h = uheap_alloc(sizeof(*h));
    if (!h)
        return UACPI_STATUS_OUT_OF_MEMORY;
    h->base = (uint16_t)base;
    h->len = (uint64_t)len;
    *out_handle = (uacpi_handle)h;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle h) {
    uheap_free(h);
}

uacpi_status uacpi_kernel_io_read8(
    uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    struct uacpi_io_handle *io = h;
    if (!io || !v || off >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *v = io_inb((uint16_t)(io->base + off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(
    uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    struct uacpi_io_handle *io = h;
    if (!io || !v || off + 1 >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *v = io_inw((uint16_t)(io->base + off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(
    uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    struct uacpi_io_handle *io = h;
    if (!io || !v || off + 3 >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    *v = io_inl((uint16_t)(io->base + off));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(
    uacpi_handle h, uacpi_size off, uacpi_u8 v) {
    struct uacpi_io_handle *io = h;
    if (!io || off >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    io_outb((uint16_t)(io->base + off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(
    uacpi_handle h, uacpi_size off, uacpi_u16 v) {
    struct uacpi_io_handle *io = h;
    if (!io || off + 1 >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    io_outw((uint16_t)(io->base + off), v);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(
    uacpi_handle h, uacpi_size off, uacpi_u32 v) {
    struct uacpi_io_handle *io = h;
    if (!io || off + 3 >= io->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    io_outl((uint16_t)(io->base + off), v);
    return UACPI_STATUS_OK;
}

static uint8_t g_uacpi_early_table_buf[4096] __attribute__((aligned(8)));
static bool g_uacpi_tables_ready = false;
static bool g_uacpi_full_ready = false;

bool acpi_uacpi_tables_ready(void) {
    return g_uacpi_tables_ready;
}

bool acpi_uacpi_full_ready(void) {
    return g_uacpi_full_ready;
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

int acpi_uacpi_full_init(void) {
    if (!g_uacpi_tables_ready)
        return -1;
    uacpi_status st = uacpi_initialize(0);
    if (st != UACPI_STATUS_OK) {
        klogf(KLOG_ERROR, "acpi: uACPI initialize failed: %s",
              uacpi_status_to_string(st));
        return -2;
    }
    st = uacpi_namespace_load();
    if (st != UACPI_STATUS_OK) {
        klogf(KLOG_ERROR, "acpi: uACPI namespace_load failed: %s",
              uacpi_status_to_string(st));
        return -3;
    }
    st = uacpi_namespace_initialize();
    if (st != UACPI_STATUS_OK) {
        klogf(KLOG_ERROR, "acpi: uACPI namespace_initialize failed: %s",
              uacpi_status_to_string(st));
        return -4;
    }
    g_uacpi_full_ready = true;
    klog(KLOG_OK, "acpi: uACPI full AML namespace ready");
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

static uacpi_iteration_decision uacpi_dump_dev(
    void *ctx, uacpi_namespace_node *node, uacpi_u32 depth) {
    (void)ctx;
    (void)depth;
    uacpi_id_string *hid = NULL;
    if (uacpi_eval_hid(node, &hid) != UACPI_STATUS_OK)
        return UACPI_ITERATION_DECISION_CONTINUE;
    klogf(KLOG_INFO, "acpi: dev %s", hid->value ? hid->value : "?");
    uacpi_free_id_string(hid);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_uacpi_rescan_tables(void) {
    if (!g_uacpi_tables_ready)
        return;
    uacpi_size n = uacpi_table_count();
    for (uacpi_size i = 0; i < n; i++) {
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
        if ((s[0] == 'D' && s[1] == 'S' && s[2] == 'D' && s[3] == 'T') ||
            (s[0] == 'S' && s[1] == 'S' && s[2] == 'D' && s[3] == 'T'))
            acpi_scan_blob((const uint8_t *)tbl.ptr, tbl.hdr->length, s);
        uacpi_table_unref(&tbl);
    }
}

void acpi_uacpi_dump(void) {
    if (!g_uacpi_tables_ready) {
        klog(KLOG_WARN, "acpi: uACPI tables not ready");
        return;
    }
    uacpi_size n = uacpi_table_count();
    klogf(KLOG_INFO, "acpi: uACPI tables=%u full=%d", (unsigned)n,
          g_uacpi_full_ready ? 1 : 0);
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
    if (g_uacpi_full_ready)
        uacpi_namespace_for_each_child(uacpi_namespace_root(), uacpi_dump_dev,
            NULL, UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, NULL);
}
