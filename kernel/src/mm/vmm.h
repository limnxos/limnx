#ifndef LIMNX_VMM_H
#define LIMNX_VMM_H

#include <stdint.h>
#include "arch/pte.h"

void     vmm_init(void);
int      vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_phys(uint64_t virt);

/* Per-process address space support */
uint64_t vmm_create_address_space(void);
int      vmm_map_page_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_get_kernel_pml4(void);

/* COW support */
uint64_t *vmm_get_pte(uint64_t pml4_phys, uint64_t virt);

/* Forward declaration to avoid circular include with process.h */
struct mmap_entry;
uint64_t  vmm_clone_cow(uint64_t parent_cr3,
                         struct mmap_entry *mmap_table, int mmap_count);
void      vmm_free_user_pages(uint64_t cr3);

/* TLB shootdown: flush TLB on all other CPUs via IPI */
void      vmm_tlb_shootdown(void);

#endif
