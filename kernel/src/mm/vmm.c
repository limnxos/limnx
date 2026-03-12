#define pr_fmt(fmt) "[vmm]  " fmt
#include "klog.h"

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/swap.h"
#include "proc/process.h"
#include "serial.h"
#include "arch/paging.h"

/* PML4 physical address, read from CR3 */
static uint64_t pml4_phys;

/* Zero a page-table page (512 entries of 8 bytes) */
static void zero_page(void *page) {
    uint64_t *p = (uint64_t *)page;
    for (int i = 0; i < 512; i++)
        p[i] = 0;
}

/*
 * Walk one level of the page table hierarchy.
 * If the entry is not present and create==true, allocate a new table page.
 * Returns virtual pointer to the next-level table, or NULL on failure.
 */
static uint64_t *get_or_create_table(uint64_t *table, uint64_t index, int create) {
    if (table[index] & PTE_PRESENT) {
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        return (uint64_t *)PHYS_TO_VIRT(next_phys);
    }

    if (!create)
        return NULL;

    /* Allocate a new page for the next-level table */
    uint64_t new_page = pmm_alloc_page();
    if (new_page == 0)
        return NULL;

    uint64_t *new_table = (uint64_t *)PHYS_TO_VIRT(new_page);
    zero_page(new_table);

    /* Install entry: present + writable (intermediate entries need both) */
    table[index] = new_page | PTE_PRESENT | PTE_WRITABLE;

    return new_table;
}

void vmm_init(void) {
    pml4_phys = arch_get_address_space() & PTE_ADDR_MASK;
    pr_info("PML4 at phys %lx\n", pml4_phys);
    pr_info("Virtual memory manager initialized\n");
}

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    uint64_t *pdpt = get_or_create_table(pml4, PML4_INDEX(virt), 1);
    if (!pdpt) return -1;

    uint64_t *pd = get_or_create_table(pdpt, PDPT_INDEX(virt), 1);
    if (!pd) return -1;

    uint64_t *pt = get_or_create_table(pd, PD_INDEX(virt), 1);
    if (!pt) return -1;

    /* Set leaf PTE */
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    arch_flush_tlb_page(virt);
    return 0;
}

void vmm_unmap_page(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    uint64_t *pdpt = get_or_create_table(pml4, PML4_INDEX(virt), 0);
    if (!pdpt) return;

    uint64_t *pd = get_or_create_table(pdpt, PDPT_INDEX(virt), 0);
    if (!pd) return;

    uint64_t *pt = get_or_create_table(pd, PD_INDEX(virt), 0);
    if (!pt) return;

    pt[PT_INDEX(virt)] = 0;
    arch_flush_tlb_page(virt);
}

uint64_t vmm_get_phys(uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    uint64_t *pdpt = get_or_create_table(pml4, PML4_INDEX(virt), 0);
    if (!pdpt) return 0;

    uint64_t *pd = get_or_create_table(pdpt, PDPT_INDEX(virt), 0);
    if (!pd) return 0;

    uint64_t *pt = get_or_create_table(pd, PD_INDEX(virt), 0);
    if (!pt) return 0;

    uint64_t pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT))
        return 0;

    return (pte & PTE_ADDR_MASK) | (virt & 0xFFF);
}

uint64_t vmm_get_kernel_pml4(void) {
    return pml4_phys;
}

/*
 * Create a new address space: allocate a PML4, zero it, then copy
 * the kernel half (entries 256–511) from the kernel PML4.
 * Returns the physical address of the new PML4, or 0 on failure.
 */
uint64_t vmm_create_address_space(void) {
    uint64_t new_pml4_phys = pmm_alloc_page();
    if (new_pml4_phys == 0)
        return 0;

    uint64_t *new_pml4 = (uint64_t *)PHYS_TO_VIRT(new_pml4_phys);
    uint64_t *kern_pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    /* Zero user half (entries 0–255) */
    for (int i = 0; i < 256; i++)
        new_pml4[i] = 0;

    /* Copy kernel half (entries 256–511) */
    for (int i = 256; i < 512; i++)
        new_pml4[i] = kern_pml4[i];

    return new_pml4_phys;
}

/*
 * Map a page in a specific address space (identified by pml4_phys).
 * Same as vmm_map_page() but operates on an arbitrary PML4.
 */
int vmm_map_page_in(uint64_t target_pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_pml4_phys);

    /* For user-space mappings, intermediate entries also need PTE_USER */
    uint64_t intermediate_flags = PTE_PRESENT | PTE_WRITABLE;
    if (flags & PTE_USER)
        intermediate_flags |= PTE_USER;

    /* PML4 → PDPT */
    uint64_t pml4_idx = PML4_INDEX(virt);
    if (!(pml4[pml4_idx] & PTE_PRESENT)) {
        uint64_t new_page = pmm_alloc_page();
        if (new_page == 0) return -1;
        uint64_t *t = (uint64_t *)PHYS_TO_VIRT(new_page);
        zero_page(t);
        pml4[pml4_idx] = new_page | intermediate_flags;
    } else {
        /* Ensure USER bit is set if needed */
        if ((flags & PTE_USER) && !(pml4[pml4_idx] & PTE_USER))
            pml4[pml4_idx] |= PTE_USER;
    }
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

    /* PDPT → PD */
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        uint64_t new_page = pmm_alloc_page();
        if (new_page == 0) return -1;
        uint64_t *t = (uint64_t *)PHYS_TO_VIRT(new_page);
        zero_page(t);
        pdpt[pdpt_idx] = new_page | intermediate_flags;
    } else {
        if ((flags & PTE_USER) && !(pdpt[pdpt_idx] & PTE_USER))
            pdpt[pdpt_idx] |= PTE_USER;
    }
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    /* PD → PT */
    uint64_t pd_idx = PD_INDEX(virt);
    if (!(pd[pd_idx] & PTE_PRESENT)) {
        uint64_t new_page = pmm_alloc_page();
        if (new_page == 0) return -1;
        uint64_t *t = (uint64_t *)PHYS_TO_VIRT(new_page);
        zero_page(t);
        pd[pd_idx] = new_page | intermediate_flags;
    } else {
        if ((flags & PTE_USER) && !(pd[pd_idx] & PTE_USER))
            pd[pd_idx] |= PTE_USER;
    }
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    /* Set leaf PTE */
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    return 0;
}

/*
 * Get pointer to leaf PTE for a virtual address in a given address space.
 * Returns NULL if any intermediate table is not present.
 */
uint64_t *vmm_get_pte(uint64_t target_pml4_phys, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_pml4_phys);

    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT)) return NULL;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);

    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT)) return NULL;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);

    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT)) return NULL;
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);

    return &pt[PT_INDEX(virt)];
}

/*
 * Clone the user-half of a page table with COW semantics.
 * Both parent and child PTEs become read-only + COW.
 * Physical page refcounts are incremented.
 * Returns child PML4 phys, or 0 on failure.
 */
/*
 * Check if a virtual address falls within a shared memory mmap region.
 * Returns 1 if the address is in a shm region, 0 otherwise.
 */
static int is_shm_page(uint64_t virt, struct mmap_entry *mmap_table, int mmap_count) {
    if (!mmap_table) return 0;
    for (int i = 0; i < mmap_count; i++) {
        if (!mmap_table[i].used) continue;
        if (mmap_table[i].shm_id < 0) continue;
        uint64_t start = mmap_table[i].virt_addr;
        uint64_t end = start + (uint64_t)mmap_table[i].num_pages * 4096;
        if (virt >= start && virt < end)
            return 1;
    }
    return 0;
}

uint64_t vmm_clone_cow(uint64_t parent_cr3,
                        struct mmap_entry *mmap_table, int mmap_count) {
    uint64_t child_cr3 = vmm_create_address_space();
    if (child_cr3 == 0) return 0;

    uint64_t *parent_pml4 = (uint64_t *)PHYS_TO_VIRT(parent_cr3);

    /* Walk user-half only (entries 0-255) */
    for (int i4 = 0; i4 < 256; i4++) {
        if (!(parent_pml4[i4] & PTE_PRESENT)) continue;
        uint64_t *parent_pdpt = (uint64_t *)PHYS_TO_VIRT(parent_pml4[i4] & PTE_ADDR_MASK);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(parent_pdpt[i3] & PTE_PRESENT)) continue;
            uint64_t *parent_pd = (uint64_t *)PHYS_TO_VIRT(parent_pdpt[i3] & PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(parent_pd[i2] & PTE_PRESENT)) continue;
                if (parent_pd[i2] & PTE_HUGE) continue;  /* skip 2MB pages */
                uint64_t *parent_pt = (uint64_t *)PHYS_TO_VIRT(parent_pd[i2] & PTE_ADDR_MASK);

                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t pte = parent_pt[i1];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t flags = pte & ~PTE_ADDR_MASK;

                    /* Reconstruct virtual address */
                    uint64_t virt = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                    ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);

                    if (is_shm_page(virt, mmap_table, mmap_count)) {
                        /* Shared memory: keep writable, no COW — both processes
                         * share the same physical page as intended */
                        vmm_map_page_in(child_cr3, virt, phys,
                                        flags & ~PTE_PRESENT);
                        pmm_ref_inc(phys);
                    } else {
                        /* Private page: mark both parent and child as COW */
                        uint64_t cow_flags = (flags & ~PTE_WRITABLE) | PTE_COW;
                        /* Track if page was originally writable (not just COW from mprotect RO) */
                        if (flags & PTE_WRITABLE)
                            cow_flags |= PTE_WAS_WRITABLE;
                        parent_pt[i1] = phys | cow_flags;

                        vmm_map_page_in(child_cr3, virt, phys,
                                        cow_flags & ~PTE_PRESENT);
                        pmm_ref_inc(phys);
                    }
                }
            }
        }
    }

    /* Flush parent's TLB since we changed parent PTEs */
    arch_switch_address_space(parent_cr3);

    return child_cr3;
}

/*
 * Free all user-half pages in an address space.
 * Decrements refcounts; only frees physical pages when refcount hits 0.
 * Also frees page table pages (PT, PD, PDPT) for the user half.
 */
void vmm_free_user_pages(uint64_t cr3) {
    uint64_t *top_pml4 = (uint64_t *)PHYS_TO_VIRT(cr3);

    for (int i4 = 0; i4 < 256; i4++) {
        if (!(top_pml4[i4] & PTE_PRESENT)) continue;
        uint64_t pdpt_phys = top_pml4[i4] & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pdpt_phys);

        for (int i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PTE_PRESENT)) continue;
            uint64_t pd_phys = pdpt[i3] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pd_phys);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PTE_PRESENT)) continue;
                if (pd[i2] & PTE_HUGE) continue;
                uint64_t pt_phys = pd[i2] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PTE_PRESENT) {
                        uint64_t page_phys = pt[i1] & PTE_ADDR_MASK;
                        pmm_free_page(page_phys);
                        pt[i1] = 0;
                    } else if (swap_is_entry(pt[i1])) {
                        swap_free_slot(swap_get_slot(pt[i1]));
                        pt[i1] = 0;
                    }
                }

                pd[i2] = 0;
                pmm_free_page(pt_phys);
            }

            pdpt[i3] = 0;
            pmm_free_page(pd_phys);
        }

        top_pml4[i4] = 0;
        pmm_free_page(pdpt_phys);
    }

    /* Free the PML4 page itself */
    pmm_free_page(cr3);
}
