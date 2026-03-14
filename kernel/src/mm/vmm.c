#define pr_fmt(fmt) "[vmm]  " fmt
#include "klog.h"

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/swap.h"
#include "proc/process.h"
#include "arch/serial.h"
#include "arch/paging.h"
#include "arch/smp_hal.h"
#include "arch/percpu.h"

/* PML4 physical address, read from CR3 */
static uint64_t pml4_phys;

/* ARM64 leaf PTEs need extra bits: page descriptor (TABLE), AF, shareability.
 * x86_64 needs TABLE (no-op = 0) and ACCESSED (bit 5, different from ARM64). */
#if defined(__aarch64__)
#define LEAF_PTE_ARCH_BITS  (PTE_TABLE | PTE_ACCESSED | PTE_SH_ISH)
#else
#define LEAF_PTE_ARCH_BITS  (PTE_TABLE | PTE_ACCESSED)
#endif

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
#if defined(__aarch64__)
        /* On ARM64, check for block descriptor (bit 1 = 0).
         * Block descriptors at L1/L2 can't be traversed as tables. */
        if (!(table[index] & PTE_TABLE))
            return NULL;  /* block descriptor — can't descend */
#endif
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

    /* Install entry: present + table + writable (intermediate entries need all) */
    table[index] = new_page | PTE_PRESENT | PTE_TABLE | PTE_WRITABLE;

    return new_table;
}

void vmm_init(void) {
#if defined(__aarch64__)
    /* ARM64: boot.S set up 1GB block descriptors for identity mapping.
     * These are incompatible with vmm_map_page_in (which needs full L0→L3
     * table hierarchy for 4KB page granularity).
     *
     * Rebuild the kernel page tables with proper 4-level hierarchy:
     *   L0[0] → L1 table
     *   L1[0] → L2 table (device MMIO range 0x00000000-0x3FFFFFFF)
     *   L1[1] → L2 table (RAM range 0x40000000-0x7FFFFFFF)
     *   L2 entries → L3 tables → 4KB page descriptors
     *
     * We use PMM to allocate the new tables, then switch TTBR0. */
    {
        #include "dtb/dtb.h"
        /* Get platform addresses from DTB (with QEMU virt defaults) */
        const dtb_platform_info_t *plat = dtb_get_platform();
        uint64_t ram_base   = (plat && plat->valid) ? plat->ram_base : 0x40000000ULL;
        uint64_t ram_size   = (plat && plat->valid) ? plat->ram_size : (256ULL * 1024 * 1024);
        uint64_t gic_base   = (plat && plat->valid) ? plat->gic_dist_base : 0x08000000ULL;
        uint64_t uart_base  = (plat && plat->valid) ? plat->uart_base : 0x09000000ULL;
        uint64_t vmio_base  = (plat && plat->valid) ? plat->virtio_mmio_base : 0x0A000000ULL;

        #define L2_BLOCK_DEV(addr) \
            ((addr) | PTE_PRESENT | PTE_ATTRINDX_DEVICE | PTE_ACCESSED | PTE_SH_ISH)

        /* Allocate L0 table */
        uint64_t l0_phys = pmm_alloc_page();
        uint64_t *l0 = (uint64_t *)PHYS_TO_VIRT(l0_phys);
        zero_page(l0);

        /* Allocate L1 table for L0[0] (covers 0x0-0x7FFFFFFFFF) */
        uint64_t l1_phys = pmm_alloc_page();
        uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);
        zero_page(l1);
        l0[0] = l1_phys | PTE_PRESENT | PTE_TABLE;

        /* L1[0]: device range 0x00000000-0x3FFFFFFF — allocate L2 table */
        uint64_t l2_dev_phys = pmm_alloc_page();
        uint64_t *l2_dev = (uint64_t *)PHYS_TO_VIRT(l2_dev_phys);
        zero_page(l2_dev);
        l1[0] = l2_dev_phys | PTE_PRESENT | PTE_TABLE;

        /* Map device MMIO as 2MB blocks in L2 (addresses from DTB) */
        l2_dev[gic_base >> 21] = L2_BLOCK_DEV(gic_base & ~0x1FFFFFULL);
        l2_dev[(gic_base + 0x200000) >> 21] = L2_BLOCK_DEV((gic_base + 0x200000) & ~0x1FFFFFULL);
        l2_dev[uart_base >> 21] = L2_BLOCK_DEV(uart_base & ~0x1FFFFFULL);
        l2_dev[vmio_base >> 21] = L2_BLOCK_DEV(vmio_base & ~0x1FFFFFULL);

        /* L1[1]: RAM range — allocate L2 table */
        uint64_t l1_ram_idx = ram_base >> 30;  /* L1 index for RAM base */
        uint64_t l2_ram_phys = pmm_alloc_page();
        uint64_t *l2_ram = (uint64_t *)PHYS_TO_VIRT(l2_ram_phys);
        zero_page(l2_ram);
        l1[l1_ram_idx] = l2_ram_phys | PTE_PRESENT | PTE_TABLE;

        /* Map RAM using full L3 page tables (4KB pages) */
        uint64_t num_2mb_regions = ram_size / (2ULL * 1024 * 1024);
        for (uint64_t i = 0; i < num_2mb_regions; i++) {
            uint64_t l3_phys = pmm_alloc_page();
            if (l3_phys == 0) {
                pr_err("out of memory for L3 table %lu\n", i);
                break;
            }
            uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
            for (int j = 0; j < 512; j++) {
                uint64_t pa = ram_base + i * (2ULL * 1024 * 1024) + j * 4096;
                l3[j] = pa | PTE_PRESENT | PTE_TABLE
                       | PTE_ATTRINDX_NORMAL | PTE_ACCESSED | PTE_SH_ISH;
            }
            l2_ram[i] = l3_phys | PTE_PRESENT | PTE_TABLE;
        }

        /* Switch to new page tables */
        pml4_phys = l0_phys;
        __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(l0_phys));
        __asm__ volatile ("tlbi vmalle1is");
        __asm__ volatile ("dsb sy");
        __asm__ volatile ("isb");
    }
#else
    pml4_phys = arch_get_address_space() & PTE_ADDR_MASK;
#endif
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

    /* Set leaf PTE (PTE_TABLE for ARM64 L3 page desc, PTE_ACCESSED for ARM64 AF) */
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT | LEAF_PTE_ARCH_BITS;

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

#if defined(__aarch64__)
    /* ARM64: kernel runs identity-mapped in the low half (0x40080000).
     * Each process needs its own L1/L2 tables so vmm_map_page_in can
     * create L3 page tables for user pages without corrupting shared tables.
     *
     * We deep-copy the kernel's L1 and L2 tables. L2 entries are 2MB block
     * descriptors that don't need further splitting for kernel mappings.
     * vmm_map_page_in creates L3 tables in empty L2 slots for user pages. */
    for (int i = 1; i < 512; i++)
        new_pml4[i] = 0;

    /* Deep-copy L0[0] → L1 → L2 tables */
    if (kern_pml4[0] & PTE_PRESENT) {
        uint64_t *kern_l1 = (uint64_t *)PHYS_TO_VIRT(kern_pml4[0] & PTE_ADDR_MASK);

        uint64_t new_l1_phys = pmm_alloc_page();
        if (new_l1_phys == 0) { pmm_free_page(new_pml4_phys); return 0; }
        uint64_t *new_l1 = (uint64_t *)PHYS_TO_VIRT(new_l1_phys);

        for (int i = 0; i < 512; i++) {
            if (!(kern_l1[i] & PTE_PRESENT)) {
                new_l1[i] = 0;
                continue;
            }
            /* Check if L1 entry is a table descriptor (bit 1 set) */
            if (kern_l1[i] & PTE_TABLE) {
                /* Deep-copy L2 table */
                uint64_t *kern_l2 = (uint64_t *)PHYS_TO_VIRT(kern_l1[i] & PTE_ADDR_MASK);
                uint64_t new_l2_phys = pmm_alloc_page();
                if (new_l2_phys == 0) {
                    pmm_free_page(new_l1_phys);
                    pmm_free_page(new_pml4_phys);
                    return 0;
                }
                uint64_t *new_l2 = (uint64_t *)PHYS_TO_VIRT(new_l2_phys);
                for (int j = 0; j < 512; j++)
                    new_l2[j] = kern_l2[j];  /* Copy 2MB block descs as-is */
                new_l1[i] = new_l2_phys | (kern_l1[i] & ~PTE_ADDR_MASK);
            } else {
                /* Block descriptor — copy as-is (shouldn't happen with new vmm_init) */
                new_l1[i] = kern_l1[i];
            }
        }
        new_pml4[0] = new_l1_phys | (kern_pml4[0] & ~PTE_ADDR_MASK);
    } else {
        new_pml4[0] = 0;
    }
#else
    /* x86_64: user half (entries 0-255), kernel half (entries 256-511) */
    for (int i = 0; i < 256; i++)
        new_pml4[i] = 0;
    for (int i = 256; i < 512; i++)
        new_pml4[i] = kern_pml4[i];
#endif

    return new_pml4_phys;
}

/*
 * Map a page in a specific address space (identified by pml4_phys).
 * Same as vmm_map_page() but operates on an arbitrary PML4.
 */
int vmm_map_page_in(uint64_t target_pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(target_pml4_phys);

    /* For user-space mappings, intermediate entries also need PTE_USER.
     * ARM64 intermediate (table) descriptors need PTE_TABLE bit set. */
    uint64_t intermediate_flags = PTE_PRESENT | PTE_TABLE | PTE_WRITABLE;
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

    /* Set leaf PTE (PTE_TABLE for ARM64 L3 page desc, PTE_ACCESSED for ARM64 AF) */
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT | LEAF_PTE_ARCH_BITS;

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

#if defined(__aarch64__)
    /*
     * ARM64: kernel and user share L0[0]. Walk the deep-copied L1→L2→L3
     * tables, skipping block descriptors and kernel-shared L3 tables.
     *
     */
    uint64_t *kern_l0 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    if (parent_pml4[0] & PTE_PRESENT) {
        uint64_t *parent_l1 = (uint64_t *)PHYS_TO_VIRT(parent_pml4[0] & PTE_ADDR_MASK);

        uint64_t *kern_l1 = NULL;
        if (kern_l0[0] & PTE_PRESENT)
            kern_l1 = (uint64_t *)PHYS_TO_VIRT(kern_l0[0] & PTE_ADDR_MASK);

        for (int i1 = 0; i1 < 512; i1++) {
            if (!(parent_l1[i1] & PTE_PRESENT)) continue;
            if (!(parent_l1[i1] & PTE_TABLE)) continue;

            uint64_t *parent_l2 = (uint64_t *)PHYS_TO_VIRT(parent_l1[i1] & PTE_ADDR_MASK);

            uint64_t *kern_l2 = NULL;
            if (kern_l1 && (kern_l1[i1] & PTE_PRESENT) && (kern_l1[i1] & PTE_TABLE))
                kern_l2 = (uint64_t *)PHYS_TO_VIRT(kern_l1[i1] & PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(parent_l2[i2] & PTE_PRESENT)) continue;
                if (!(parent_l2[i2] & PTE_TABLE)) continue;
                if (kern_l2 && parent_l2[i2] == kern_l2[i2]) continue;

                uint64_t *parent_pt = (uint64_t *)PHYS_TO_VIRT(parent_l2[i2] & PTE_ADDR_MASK);

                for (int i3 = 0; i3 < 512; i3++) {
                    uint64_t pte = parent_pt[i3];
                    if (!(pte & PTE_PRESENT)) continue;

                    uint64_t phys = pte & PTE_ADDR_MASK;
                    uint64_t flags = pte & ~PTE_ADDR_MASK;

                    uint64_t virt = ((uint64_t)i1 << 30) |
                                    ((uint64_t)i2 << 21) | ((uint64_t)i3 << 12);

                    if (is_shm_page(virt, mmap_table, mmap_count)) {
                        vmm_map_page_in(child_cr3, virt, phys,
                                        flags & ~PTE_PRESENT);
                        pmm_ref_inc(phys);
                    } else {
                        uint64_t cow_flags = PTE_MAKE_READONLY(flags) | PTE_COW;
                        if (PTE_IS_WRITABLE(flags))
                            cow_flags |= PTE_WAS_WRITABLE;
                        parent_pt[i3] = phys | cow_flags;

                        vmm_map_page_in(child_cr3, virt, phys,
                                        cow_flags & ~PTE_PRESENT);
                        pmm_ref_inc(phys);
                    }
                }
            }
        }
    }
#else
    /* x86_64: Walk user-half only (entries 0-255) */
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
#endif

    /* Flush parent's TLB since we changed parent PTEs to COW */
    arch_switch_address_space(parent_cr3);
    /* Flush TLB on all other CPUs too (SMP COW correctness) */
    vmm_tlb_shootdown();

    return child_cr3;
}

/*
 * Free all user-half pages in an address space.
 * Decrements refcounts; only frees physical pages when refcount hits 0.
 * Also frees page table pages (PT, PD, PDPT) for the user half.
 */
void vmm_free_user_pages(uint64_t cr3) {
    uint64_t *top_pml4 = (uint64_t *)PHYS_TO_VIRT(cr3);

#if defined(__aarch64__)
    /*
     * ARM64: kernel and user pages share L0[0]. The process has deep-copied
     * L1 and L2 tables from the kernel. We must:
     *  - Skip L2 block descriptors (device MMIO — no PTE_TABLE bit)
     *  - Skip L2 table entries that point to kernel-shared L3 tables
     *  - Only walk/free L3 tables created by vmm_map_page_in for user pages
     *  - Free the deep-copied L1 and L2 pages
     */
    uint64_t *kern_l0 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);

    if (top_pml4[0] & PTE_PRESENT) {
        uint64_t l1_phys = top_pml4[0] & PTE_ADDR_MASK;
        uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);

        /* Get kernel L1 for comparison */
        uint64_t *kern_l1 = NULL;
        if (kern_l0[0] & PTE_PRESENT)
            kern_l1 = (uint64_t *)PHYS_TO_VIRT(kern_l0[0] & PTE_ADDR_MASK);

        for (int i1 = 0; i1 < 512; i1++) {
            if (!(l1[i1] & PTE_PRESENT)) continue;
            if (!(l1[i1] & PTE_TABLE)) continue;  /* skip L1 block descriptors */

            uint64_t l2_phys = l1[i1] & PTE_ADDR_MASK;
            uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);

            /* Get corresponding kernel L2 table for comparison */
            uint64_t *kern_l2 = NULL;
            if (kern_l1 && (kern_l1[i1] & PTE_PRESENT) && (kern_l1[i1] & PTE_TABLE))
                kern_l2 = (uint64_t *)PHYS_TO_VIRT(kern_l1[i1] & PTE_ADDR_MASK);

            for (int i2 = 0; i2 < 512; i2++) {
                if (!(l2[i2] & PTE_PRESENT)) continue;
                if (!(l2[i2] & PTE_TABLE)) continue;  /* skip block descriptors */

                /* If this L2 entry matches the kernel's, it's a shared L3 — skip */
                if (kern_l2 && l2[i2] == kern_l2[i2]) continue;

                uint64_t pt_phys = l2[i2] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);

                for (int i3 = 0; i3 < 512; i3++) {
                    if (pt[i3] & PTE_PRESENT) {
                        pmm_free_page(pt[i3] & PTE_ADDR_MASK);
                        pt[i3] = 0;
                    } else if (swap_is_entry(pt[i3])) {
                        swap_free_slot(swap_get_slot(pt[i3]));
                        pt[i3] = 0;
                    }
                }
                pmm_free_page(pt_phys);
            }

            /* Free the deep-copied L2 page */
            pmm_free_page(l2_phys);
        }

        /* Free the deep-copied L1 page */
        pmm_free_page(l1_phys);
    }

    /* Free the L0 page itself */
    pmm_free_page(cr3);
#else
    /* x86_64: user half is entries 0-255 */
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
#endif
}

/*
 * TLB shootdown: send IPI to all other CPUs to flush their TLBs.
 * Used after COW PTE modifications to ensure stale writable mappings
 * don't persist on other cores.
 */
void vmm_tlb_shootdown(void) {
    arch_tlb_shootdown();
}
