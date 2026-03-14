#define pr_fmt(fmt) "[pmm]  " fmt
#include "klog.h"

#include "mm/pmm.h"
#include "sync/spinlock.h"
#include "arch/serial.h"
#if defined(__x86_64__)
#include "limine.h"
#endif

/*
 * Lock ordering: pmm_lock is at level 2.
 * Must NOT hold sched_lock when acquiring.
 * May be acquired while kheap_lock is NOT held (kheap calls pmm).
 * May call kmalloc while holding this lock: NO
 */
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* HHDM offset — set during pmm_init from Limine response */
uint64_t hhdm_offset = 0;

/* Bitmap: 1 bit per 4KB page frame. Bit set = page used. */
static uint8_t *bitmap;
static uint64_t bitmap_size;    /* in bytes */
static uint64_t total_pages;
static uint64_t free_pages;

/* Reference counts: one uint16_t per page frame (for COW) */
static uint16_t *refcounts;

/* Allocation hint: start scanning from here for faster contiguous alloc */
static uint64_t alloc_hint = 1;

#if defined(__x86_64__)
/* Limine requests (referenced from main.c via extern) */
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_hhdm_request   hhdm_request;
#endif

static inline void bitmap_set(uint64_t page) {
    bitmap[page / 8] |= (1 << (page % 8));
}

static inline void bitmap_clear(uint64_t page) {
    bitmap[page / 8] &= ~(1 << (page % 8));
}

static inline int bitmap_test(uint64_t page) {
    return (bitmap[page / 8] >> (page % 8)) & 1;
}

void pmm_init(void) {
#if defined(__aarch64__)
    /* ARM64: identity-mapped (hhdm_offset=0).
     * RAM base/size from DTB (fallback: QEMU virt defaults).
     * Kernel loaded at ram_base+0x80000. Reserve first 4MB for kernel+stack. */
    hhdm_offset = 0;

    #include "dtb/dtb.h"
    uint64_t ARM64_RAM_BASE = 0x40000000ULL;
    uint64_t ARM64_RAM_SIZE = 256ULL * 1024 * 1024;
    {
        const dtb_platform_info_t *plat = dtb_get_platform();
        if (plat && plat->valid) {
            ARM64_RAM_BASE = plat->ram_base;
            ARM64_RAM_SIZE = plat->ram_size;
        }
    }
    uint64_t ARM64_KERN_END = ARM64_RAM_BASE + 0x800000ULL;  /* first 8MB reserved (DTB+kernel+BSS) */

    uint64_t highest_addr = ARM64_RAM_BASE + ARM64_RAM_SIZE;
    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8;

    /* Place bitmap at KERN_END */
    bitmap = (uint8_t *)ARM64_KERN_END;

    /* Mark all pages as used */
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;
    free_pages = 0;

    /* Mark usable region as free: from after bitmap+refcounts to RAM end */
    uint64_t bm_phys = (uint64_t)bitmap;
    uint64_t bm_end = (bm_phys + bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* Refcount array right after bitmap */
    uint64_t refcount_bytes = total_pages * sizeof(uint16_t);
    uint64_t refcount_phys = bm_end;
    refcounts = (uint16_t *)refcount_phys;
    uint64_t rc_end = (refcount_phys + refcount_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);

    /* Zero all refcounts */
    for (uint64_t i = 0; i < total_pages; i++)
        refcounts[i] = 0;

    /* Free pages from after refcounts to RAM end */
    uint64_t free_start = rc_end / PAGE_SIZE;
    uint64_t free_end = highest_addr / PAGE_SIZE;
    for (uint64_t p = free_start; p < free_end; p++) {
        bitmap_clear(p);
        free_pages++;
    }

    /* Set refcount=1 for all used pages */
    for (uint64_t i = 1; i < total_pages; i++) {
        if (bitmap_test(i))
            refcounts[i] = 1;
    }

#else /* x86_64 */
    if (!hhdm_request.response || !memmap_request.response) {
        panic("missing HHDM or memmap response");
    }

    hhdm_offset = hhdm_request.response->offset;

    struct limine_memmap_entry **entries = memmap_request.response->entries;
    uint64_t entry_count = memmap_request.response->entry_count;

    /* Pass 1: find highest address to determine bitmap size */
    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < entry_count; i++) {
        uint64_t top = entries[i]->base + entries[i]->length;
        if (top > highest_addr)
            highest_addr = top;
    }

    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = (total_pages + 7) / 8;

    /* Pass 2: find first USABLE region large enough for bitmap */
    bitmap = NULL;
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type == LIMINE_MEMMAP_USABLE &&
            entries[i]->length >= bitmap_size) {
            bitmap = (uint8_t *)PHYS_TO_VIRT(entries[i]->base);
            break;
        }
    }

    if (!bitmap) {
        panic("no region large enough for bitmap");
    }

    /* Mark all pages as used */
    for (uint64_t i = 0; i < bitmap_size; i++)
        bitmap[i] = 0xFF;

    free_pages = 0;

    /* Pass 3: mark USABLE regions as free */
    for (uint64_t i = 0; i < entry_count; i++) {
        if (entries[i]->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t base = entries[i]->base;
        uint64_t len  = entries[i]->length;
        uint64_t start_page = base / PAGE_SIZE;
        uint64_t page_count = len / PAGE_SIZE;

        for (uint64_t p = start_page; p < start_page + page_count; p++) {
            bitmap_clear(p);
            free_pages++;
        }
    }

    /* Re-mark page 0 as used (null pointer guard) */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        free_pages--;
    }

    /*
     * NOTE: Kernel pages are NOT re-marked here because Limine's memory map
     * already reports them as KERNEL_AND_MODULES (not USABLE), so they are
     * never freed in Pass 3.  The kernel virtual address (0xFFFFFFFF80000000)
     * is NOT in the HHDM range, so VIRT_TO_PHYS() cannot be used on it.
     */

    /* Re-mark bitmap region itself as used */
    uint64_t bm_phys = VIRT_TO_PHYS((uint64_t)bitmap);
    uint64_t bm_start_page = bm_phys / PAGE_SIZE;
    uint64_t bm_end_page   = (bm_phys + bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = bm_start_page; p < bm_end_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            free_pages--;
        }
    }

    /* Allocate refcount array right after bitmap */
    uint64_t refcount_bytes = total_pages * sizeof(uint16_t);
    uint64_t refcount_phys = bm_phys + ((bitmap_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    refcounts = (uint16_t *)PHYS_TO_VIRT(refcount_phys);

    /* Zero all refcounts */
    for (uint64_t i = 0; i < total_pages; i++)
        refcounts[i] = 0;

    /* Mark refcount pages as used */
    uint64_t rc_start_page = refcount_phys / PAGE_SIZE;
    uint64_t rc_end_page   = (refcount_phys + refcount_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t p = rc_start_page; p < rc_end_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            free_pages--;
        }
    }

    /* Set refcount=1 for all already-used pages */
    for (uint64_t i = 1; i < total_pages; i++) {
        if (bitmap_test(i))
            refcounts[i] = 1;
    }
#endif /* __x86_64__ */

    pr_info("Total pages: %lu (%lu MB)\n",
        total_pages, (total_pages * PAGE_SIZE) / (1024 * 1024));
    pr_info("Free pages:  %lu (%lu MB)\n",
        free_pages, (free_pages * PAGE_SIZE) / (1024 * 1024));
    pr_info("Bitmap at %p (%lu bytes)\n",
        (void *)bitmap, bitmap_size);
}

uint64_t pmm_alloc_page(void) {
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    /* First try from hint */
    for (uint64_t i = alloc_hint; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            alloc_hint = i + 1;
            refcounts[i] = 1;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return i * PAGE_SIZE;
        }
    }
    /* Fall back to scanning from page 1 */
    for (uint64_t i = 1; i < alloc_hint; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            alloc_hint = i + 1;
            refcounts[i] = 1;
            spin_unlock_irqrestore(&pmm_lock, flags);
            return i * PAGE_SIZE;
        }
    }
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0; /* out of memory — 0 is always reserved */
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    if (count == 0)
        return 0;
    if (count == 1)
        return pmm_alloc_page();

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    uint64_t run = 0;
    uint64_t start = 0;

    /* First try from hint */
    uint64_t begin = alloc_hint > 0 ? alloc_hint : 1;
    for (uint64_t i = begin; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            if (run == 0)
                start = i;
            run++;
            if (run == count) {
                for (uint64_t j = start; j < start + count; j++) {
                    bitmap_set(j);
                    free_pages--;
                    refcounts[j] = 1;
                }
                alloc_hint = start + count;
                spin_unlock_irqrestore(&pmm_lock, flags);
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }

    /* Fall back to scanning from page 1 */
    if (begin > 1) {
        run = 0;
        for (uint64_t i = 1; i < begin; i++) {
            if (!bitmap_test(i)) {
                if (run == 0)
                    start = i;
                run++;
                if (run == count) {
                    for (uint64_t j = start; j < start + count; j++) {
                        bitmap_set(j);
                        free_pages--;
                        refcounts[j] = 1;
                    }
                    alloc_hint = start + count;
                    spin_unlock_irqrestore(&pmm_lock, flags);
                    return start * PAGE_SIZE;
                }
            } else {
                run = 0;
            }
        }
    }

    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0; /* not enough contiguous memory */
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;

    /* Guard: never free page 0 */
    if (page == 0)
        return;

    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    /* Guard: double-free check */
    if (!bitmap_test(page)) {
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }

    /* Respect refcounts: only free when last reference drops */
    if (refcounts[page] > 1) {
        refcounts[page]--;
        spin_unlock_irqrestore(&pmm_lock, flags);
        return;
    }

    refcounts[page] = 0;
    bitmap_clear(page);
    free_pages++;
    spin_unlock_irqrestore(&pmm_lock, flags);
}

void pmm_ref_inc(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page > 0 && page < total_pages) {
        uint64_t flags;
        spin_lock_irqsave(&pmm_lock, &flags);
        refcounts[page]++;
        spin_unlock_irqrestore(&pmm_lock, flags);
    }
}

int pmm_ref_dec(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page == 0 || page >= total_pages) return 0;
    uint64_t flags;
    spin_lock_irqsave(&pmm_lock, &flags);
    if (refcounts[page] > 0) refcounts[page]--;
    int rc = refcounts[page];
    spin_unlock_irqrestore(&pmm_lock, flags);
    return rc;
}

uint16_t pmm_ref_get(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page >= total_pages) return 0;
    return refcounts[page];
}

uint64_t pmm_get_total_pages(void) {
    return total_pages;
}

uint64_t pmm_get_free_pages(void) {
    return free_pages;
}
