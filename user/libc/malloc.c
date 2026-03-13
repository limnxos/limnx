#include "libc.h"

#define ALIGNMENT    16
#define HEADER_SIZE  32  /* sizeof(block_header_t) */
#define MIN_EXPAND   (4 * PAGE_SIZE)  /* minimum 16KB per mmap expansion */

typedef struct block_header {
    uint64_t size;               /* total block size including header */
    uint64_t is_free;
    struct block_header *next;
    struct block_header *prev;
} block_header_t;

static block_header_t *heap_head;

static uint64_t align_up(uint64_t val, uint64_t align) {
    return (val + align - 1) & ~(align - 1);
}

/*
 * Expand the heap by allocating new pages via sys_mmap.
 * Returns a pointer to a new free block, or NULL on failure.
 */
static block_header_t *heap_expand(uint64_t min_size) {
    uint64_t alloc_size = min_size;
    if (alloc_size < MIN_EXPAND)
        alloc_size = MIN_EXPAND;

    uint64_t pages = (alloc_size + PAGE_SIZE - 1) / PAGE_SIZE;
    long addr = sys_mmap(pages);
    if (addr <= 0)
        return NULL;

    block_header_t *block = (block_header_t *)addr;
    block->size = pages * PAGE_SIZE;
    block->is_free = 1;
    block->next = NULL;
    block->prev = NULL;

    /* Append to the end of the block list */
    if (!heap_head) {
        heap_head = block;
    } else {
        block_header_t *tail = heap_head;
        while (tail->next)
            tail = tail->next;

        /* Check if new region is adjacent to tail — coalesce */
        if ((uint64_t)tail + tail->size == (uint64_t)block && tail->is_free) {
            tail->size += block->size;
            return tail;
        }

        tail->next = block;
        block->prev = tail;
    }

    return block;
}

void *malloc(size_t size) {
    if (size == 0)
        return NULL;

    uint64_t total_needed = align_up(size + HEADER_SIZE, ALIGNMENT);
    if (total_needed < HEADER_SIZE + ALIGNMENT)
        total_needed = HEADER_SIZE + ALIGNMENT;

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
            return (void *)((uint64_t)block + HEADER_SIZE);
        }
        block = block->next;
    }

    /* No suitable block — expand */
    block_header_t *new_block = heap_expand(total_needed);
    if (!new_block)
        return NULL;

    /* Retry — the new block should satisfy the request */
    return malloc(size);
}

void free(void *ptr) {
    if (!ptr)
        return;

    block_header_t *block = (block_header_t *)((uint64_t)ptr - HEADER_SIZE);
    block->is_free = 1;

    /* Coalesce with next block if free and adjacent */
    if (block->next && block->next->is_free &&
        (uint64_t)block + block->size == (uint64_t)block->next) {
        block->size += block->next->size;
        block->next = block->next->next;
        if (block->next)
            block->next->prev = block;
    }

    /* Coalesce with previous block if free and adjacent */
    if (block->prev && block->prev->is_free &&
        (uint64_t)block->prev + block->prev->size == (uint64_t)block) {
        block->prev->size += block->size;
        block->prev->next = block->next;
        if (block->next)
            block->next->prev = block->prev;
    }
}

void *realloc(void *ptr, size_t new_size) {
    if (!ptr)
        return malloc(new_size);

    if (new_size == 0) {
        free(ptr);
        return NULL;
    }

    block_header_t *block = (block_header_t *)((uint64_t)ptr - HEADER_SIZE);
    uint64_t total_needed = align_up(new_size + HEADER_SIZE, ALIGNMENT);

    /* Current block large enough — return as-is */
    if (block->size >= total_needed)
        return ptr;

    /* Try to absorb next block if free and adjacent */
    if (block->next && block->next->is_free &&
        (uint64_t)block + block->size == (uint64_t)block->next) {
        uint64_t combined = block->size + block->next->size;
        if (combined >= total_needed) {
            block->size = combined;
            block->next = block->next->next;
            if (block->next)
                block->next->prev = block;
            return ptr;
        }
    }

    /* Allocate new, copy, free old */
    void *new_ptr = malloc(new_size);
    if (!new_ptr)
        return NULL;

    uint64_t copy_size = block->size - HEADER_SIZE;
    if (copy_size > new_size)
        copy_size = new_size;
    memcpy(new_ptr, ptr, copy_size);

    free(ptr);
    return new_ptr;
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (count != 0 && total / count != size)
        return NULL;  /* overflow */

    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}
