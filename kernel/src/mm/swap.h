#ifndef LIMNX_SWAP_H
#define LIMNX_SWAP_H

#include <stdint.h>

/* Swap area: last 8MB of disk. Start block set at runtime by swap_init_at(). */
extern uint32_t swap_start_block;    /* set dynamically from disk capacity */
#define SWAP_START_BLOCK     swap_start_block
#define SWAP_NUM_PAGES       2048    /* 8MB / 4KB */
#define SWAP_SECTORS_PER_PAGE 8      /* 4KB / 512B */

/* PTE encoding: PTE_PRESENT=0, PTE_SWAP=1, slot in bits 12-22 */
#define SWAP_SLOT_SHIFT    12
#define SWAP_SLOT_MASK     0x7FF    /* 11 bits, max 2048 slots */

void swap_init(void);
void swap_init_at(uint32_t start_block);  /* set swap area start from disk capacity */

/* Allocate a swap slot, returns slot index or -1 */
int  swap_alloc_slot(void);

/* Free a swap slot */
void swap_free_slot(int slot);

/* Swap out: write page to disk, encode swap PTE, free physical page */
int  swap_out(uint64_t pml4_phys, uint64_t virt);

/* Swap in: alloc page, read from disk, restore PTE, free slot */
int  swap_in(uint64_t pml4_phys, uint64_t virt);

/* Check if a PTE is a swap entry */
int  swap_is_entry(uint64_t pte);

/* Extract swap slot from PTE */
int  swap_get_slot(uint64_t pte);

/* Get swap statistics */
void swap_stat(uint32_t *total, uint32_t *used);

/* Demand page fault: alloc zero-filled page and map it */
int  demand_page_fault(uint64_t pml4_phys, uint64_t virt);

#endif
