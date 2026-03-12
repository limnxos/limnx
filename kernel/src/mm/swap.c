#define pr_fmt(fmt) "[swap]  " fmt
#include "klog.h"

#include "mm/swap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "blk/bcache.h"
#include "arch/serial.h"

static uint8_t swap_bitmap[SWAP_NUM_PAGES / 8];  /* 256 bytes */
static uint32_t swap_used_count;

void swap_init(void) {
    for (int i = 0; i < SWAP_NUM_PAGES / 8; i++)
        swap_bitmap[i] = 0;
    swap_used_count = 0;
    pr_info("Swap area initialized (%u pages, blocks %u-%u)\n",
                  SWAP_NUM_PAGES, SWAP_START_BLOCK, SWAP_START_BLOCK + SWAP_NUM_PAGES - 1);
}

int swap_alloc_slot(void) {
    for (int i = 0; i < SWAP_NUM_PAGES; i++) {
        int byte = i / 8;
        int bit = i % 8;
        if (!(swap_bitmap[byte] & (1 << bit))) {
            swap_bitmap[byte] |= (uint8_t)(1 << bit);
            swap_used_count++;
            return i;
        }
    }
    return -1;
}

void swap_free_slot(int slot) {
    if (slot < 0 || slot >= SWAP_NUM_PAGES) return;
    int byte = slot / 8;
    int bit = slot % 8;
    if (swap_bitmap[byte] & (1 << bit)) {
        swap_bitmap[byte] &= (uint8_t)~(1 << bit);
        if (swap_used_count > 0)
            swap_used_count--;
    }
}

int swap_out(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = vmm_get_pte(pml4_phys, virt);
    if (!pte || !(*pte & PTE_PRESENT))
        return -1;

    int slot = swap_alloc_slot();
    if (slot < 0)
        return -1;

    uint64_t phys = *pte & PTE_ADDR_MASK;

    /* Write page to disk (1 page = 1 block at 4KB) */
    uint32_t block = SWAP_START_BLOCK + (uint32_t)slot;
    uint8_t *page_data = (uint8_t *)PHYS_TO_VIRT(phys);
    bcache_write(block, page_data);

    /* Encode swap PTE: not present, PTE_SWAP set, slot encoded */
    *pte = PTE_SWAP | ((uint64_t)slot << SWAP_SLOT_SHIFT);

    /* Free physical page */
    pmm_free_page(phys);

    return 0;
}

int swap_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pte = vmm_get_pte(pml4_phys, virt);
    if (!pte)
        return -1;

    if (!swap_is_entry(*pte))
        return -1;

    int slot = swap_get_slot(*pte);

    /* Allocate new physical page */
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0)
        return -1;

    /* Read page from disk */
    uint32_t block = SWAP_START_BLOCK + (uint32_t)slot;
    uint8_t *page_data = (uint8_t *)PHYS_TO_VIRT(new_phys);
    bcache_read(block, page_data);

    /* Restore PTE */
    *pte = new_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;

    /* Free swap slot */
    swap_free_slot(slot);

    return 0;
}

int swap_is_entry(uint64_t pte) {
    return !(pte & PTE_PRESENT) && (pte & PTE_SWAP);
}

int swap_get_slot(uint64_t pte) {
    return (int)((pte >> SWAP_SLOT_SHIFT) & SWAP_SLOT_MASK);
}

void swap_stat(uint32_t *total, uint32_t *used) {
    if (total) *total = SWAP_NUM_PAGES;
    if (used) *used = swap_used_count;
}

int demand_page_fault(uint64_t pml4_phys, uint64_t virt) {
    uint64_t new_phys = pmm_alloc_page();
    if (new_phys == 0)
        return -1;

    /* Zero-fill the page */
    uint8_t *page = (uint8_t *)PHYS_TO_VIRT(new_phys);
    for (int i = 0; i < 4096; i++)
        page[i] = 0;

    /* Map it */
    return vmm_map_page_in(pml4_phys, virt & ~0xFFFULL, new_phys,
                           PTE_USER | PTE_WRITABLE | PTE_NX);
}
