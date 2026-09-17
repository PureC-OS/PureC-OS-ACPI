#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "mm/pmm.h"
#include "lib/string.h"

#define UHEAP_MAGIC 0x55484541u
#define UHEAP_ALIGN 8u

struct uheap_block {
    uint32_t magic;
    uint32_t free;
    uint64_t size;
    struct uheap_block *next;
};

static struct uheap_block *g_head = NULL;

static inline uint64_t uheap_irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void uheap_irq_restore(uint64_t f) {
    if (f & (1ULL << 9))
        __asm__ volatile("sti" ::: "memory");
}

static uint64_t uheap_align_up(uint64_t v) {
    return (v + (UHEAP_ALIGN - 1)) & ~(uint64_t)(UHEAP_ALIGN - 1);
}

static void uheap_coalesce(void) {
    struct uheap_block *b = g_head;
    while (b && b->next) {
        uint8_t *bend =
            (uint8_t *)b + sizeof(struct uheap_block) + b->size;
        if (b->free && b->next->free &&
            bend == (uint8_t *)b->next) {
            b->size += sizeof(struct uheap_block) + b->next->size;
            b->next = b->next->next;
            continue;
        }
        b = b->next;
    }
}

static void uheap_insert_sorted(struct uheap_block *nb) {
    nb->next = NULL;
    if (!g_head || (uintptr_t)nb < (uintptr_t)g_head) {
        nb->next = g_head;
        g_head = nb;
        return;
    }
    struct uheap_block *b = g_head;
    while (b->next && (uintptr_t)b->next < (uintptr_t)nb)
        b = b->next;
    nb->next = b->next;
    b->next = nb;
}

void *uheap_alloc(uint64_t size) {
    if (size == 0)
        return NULL;
    uint64_t need = uheap_align_up(size);
    uint64_t flags = uheap_irq_save();
    for (int attempt = 0; attempt < 2; attempt++) {
        struct uheap_block *b = g_head;
        while (b) {
            if (b->free && b->magic == UHEAP_MAGIC && b->size >= need) {
                uint64_t leftover = b->size - need;
                if (leftover >= sizeof(struct uheap_block) + UHEAP_ALIGN) {
                    struct uheap_block *split =
                        (struct uheap_block *)((uint8_t *)(b + 1) + need);
                    split->magic = UHEAP_MAGIC;
                    split->free = 1;
                    split->size = leftover - sizeof(struct uheap_block);
                    split->next = b->next;
                    b->size = need;
                    b->next = split;
                }
                b->free = 0;
                uheap_irq_restore(flags);
                return (void *)(b + 1);
            }
            b = b->next;
        }
        if (attempt == 0) {
            uint64_t total = need + sizeof(struct uheap_block);
            uint64_t pages = (total + 4095) / 4096;
            if (pages == 0)
                pages = 1;
            uint64_t phys = pmm_allocate_contiguous(pages);
            if (phys == 0) {
                uheap_irq_restore(flags);
                return NULL;
            }
            void *virt = pmm_physical_to_virtual(phys);
            if (!virt) {
                pmm_free_contiguous(phys, pages);
                uheap_irq_restore(flags);
                return NULL;
            }
            struct uheap_block *nb = (struct uheap_block *)virt;
            nb->magic = UHEAP_MAGIC;
            nb->free = 1;
            nb->size = pages * 4096 - sizeof(struct uheap_block);
            nb->next = NULL;
            uheap_insert_sorted(nb);
            uheap_coalesce();
        }
    }
    uheap_irq_restore(flags);
    return NULL;
}

void *uheap_alloc_zeroed(uint64_t size) {
    void *p = uheap_alloc(size);
    if (p)
        memset(p, 0, (size_t)size);
    return p;
}

void uheap_free(void *ptr) {
    if (!ptr)
        return;
    uint64_t flags = uheap_irq_save();
    struct uheap_block *b = ((struct uheap_block *)ptr) - 1;
    if (b->magic != UHEAP_MAGIC) {
        uheap_irq_restore(flags);
        return;
    }
    b->free = 1;
    uheap_coalesce();
    uheap_irq_restore(flags);
}
