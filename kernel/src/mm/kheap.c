#define pr_fmt(fmt) "[kheap] " fmt
#include "klog.h"

#include "mm/kheap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "sync/spinlock.h"
#include "arch/serial.h"

/*
 * Lock ordering: kheap_lock is at level 3.
 * Must NOT hold sched_lock when acquiring.
 * May acquire pmm_lock while held (heap_expand calls pmm_alloc_page).
 * Callers must NOT hold pmm_lock when calling kmalloc/kfree.
 */
static spinlock_t kheap_lock = SPINLOCK_INIT;

#define ALIGNMENT    16
#define HEADER_SIZE  sizeof(block_header_t)

typedef struct block_header {
    uint64_t size;              /* total block size INCLUDING header */
    uint64_t is_free;           /* 1=free, 0=allocated */
    struct block_header *next;  /* next block (address order) */
    struct block_header *prev;  /* prev block (address order) */
} block_header_t;

_Static_assert(sizeof(block_header_t) == 32, "block_header must be 32 bytes");
_Static_assert(sizeof(block_header_t) % ALIGNMENT == 0, "header must be 16-byte aligned");

static block_header_t *heap_head;
static uint64_t heap_current_end;

/* Align size up to ALIGNMENT boundary */
static inline uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

static void heap_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
}

/*
 * Expand the heap by at least min_size bytes.
 * Allocates physical pages and maps them into the heap virtual range.
 * Returns a pointer to the new free block, or NULL on failure.
 */
static block_header_t *heap_expand(uint64_t min_size) {
    uint64_t pages_needed = (min_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t expand_start = heap_current_end;

    /* Check we don't exceed max heap */
    if (expand_start + pages_needed * PAGE_SIZE > KHEAP_START + KHEAP_MAX) {
        pr_err("heap would exceed max size\n");
        return NULL;
    }

    for (uint64_t i = 0; i < pages_needed; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            pr_err("out of physical memory\n");
            return NULL;
        }
        if (vmm_map_page(heap_current_end, phys, PTE_WRITABLE | PTE_NX) != 0) {
            pr_err("vmm_map_page failed\n");
            pmm_free_page(phys);
            return NULL;
        }
        heap_current_end += PAGE_SIZE;
    }

    uint64_t total_new = pages_needed * PAGE_SIZE;

    /* Try to coalesce with previous tail block if it's free */
    block_header_t *new_block = (block_header_t *)expand_start;

    /* Find the last block in the list */
    block_header_t *tail = heap_head;
    if (tail) {
        while (tail->next)
            tail = tail->next;
    }

    if (tail && tail->is_free &&
        ((uint64_t)tail + tail->size == expand_start)) {
        /* Extend the tail block */
        tail->size += total_new;
        return tail;
    }

    /* Create a brand-new free block */
    new_block->size = total_new;
    new_block->is_free = 1;
    new_block->next = NULL;
    new_block->prev = tail;
    if (tail)
        tail->next = new_block;

    return new_block;
}

void kheap_init(void) {
    heap_current_end = KHEAP_START;
    heap_head = NULL;

    /* Expand by 1 page to bootstrap the heap */
    block_header_t *first = heap_expand(PAGE_SIZE);
    if (!first) {
        panic("initial expansion failed");
    }
    heap_head = first;

    pr_info("Heap at %lx, initial size %lu bytes\n",
        KHEAP_START, first->size);
    pr_info("Kernel heap initialized\n");
}

void *kmalloc(uint64_t size) {
    if (size == 0)
        return NULL;

    uint64_t total_needed = align_up(size + HEADER_SIZE, ALIGNMENT);
    if (total_needed < HEADER_SIZE + ALIGNMENT)
        total_needed = HEADER_SIZE + ALIGNMENT;

    uint64_t flags;
    spin_lock_irqsave(&kheap_lock, &flags);

    for (int attempt = 0; attempt < 2; attempt++) {
        /* First-fit search */
        block_header_t *block = heap_head;
        while (block) {
            if (block->is_free && block->size >= total_needed) {
                /* Split if remainder is large enough */
                uint64_t remainder = block->size - total_needed;
                if (remainder >= HEADER_SIZE + ALIGNMENT) {
                    block_header_t *split = (block_header_t *)((uint64_t)block + total_needed);
                    split->size = remainder;
                    split->is_free = 1;
                    split->next = block->next;
                    split->prev = block;
                    if (block->next)
                        block->next->prev = split;
                    block->next = split;
                    block->size = total_needed;
                }

                block->is_free = 0;
                spin_unlock_irqrestore(&kheap_lock, flags);
                return (void *)((uint64_t)block + HEADER_SIZE);
            }
            block = block->next;
        }

        /* No suitable block — expand heap (only on first attempt) */
        if (attempt == 0) {
            block_header_t *new_block = heap_expand(total_needed);
            if (!new_block) {
                spin_unlock_irqrestore(&kheap_lock, flags);
                return NULL;
            }
            if (!heap_head)
                heap_head = new_block;
            /* Loop back to retry first-fit search */
        }
    }

    spin_unlock_irqrestore(&kheap_lock, flags);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    uint64_t flags;
    spin_lock_irqsave(&kheap_lock, &flags);

    block_header_t *block = (block_header_t *)((uint64_t)ptr - HEADER_SIZE);
    block->is_free = 1;

    /* Coalesce with next block if free */
    if (block->next && block->next->is_free) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next)
            block->next->prev = block;
    }

    /* Coalesce with previous block if free */
    if (block->prev && block->prev->is_free) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next)
            block->next->prev = block->prev;
    }

    spin_unlock_irqrestore(&kheap_lock, flags);
}

void *krealloc(void *ptr, uint64_t new_size) {
    if (!ptr)
        return kmalloc(new_size);

    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }

    block_header_t *block = (block_header_t *)((uint64_t)ptr - HEADER_SIZE);
    uint64_t total_needed = align_up(new_size + HEADER_SIZE, ALIGNMENT);

    /* If current block is large enough, return as-is */
    if (block->size >= total_needed)
        return ptr;

    /* Allocate new, copy, free old */
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;

    uint64_t copy_size = block->size - HEADER_SIZE;
    if (copy_size > new_size)
        copy_size = new_size;
    heap_memcpy(new_ptr, ptr, copy_size);

    kfree(ptr);
    return new_ptr;
}
