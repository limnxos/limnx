#ifndef LIMNX_VMM_H
#define LIMNX_VMM_H

#include <stdint.h>

/* PTE flags */
#define PTE_PRESENT      (1ULL << 0)
#define PTE_WRITABLE     (1ULL << 1)
#define PTE_USER         (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE      (1ULL << 4)
#define PTE_ACCESSED     (1ULL << 5)
#define PTE_DIRTY        (1ULL << 6)
#define PTE_HUGE         (1ULL << 7)
#define PTE_GLOBAL       (1ULL << 8)
#define PTE_COW          (1ULL << 9)   /* Copy-on-write (available bit) */
#define PTE_SWAP         (1ULL << 10)  /* Page swapped to disk (available bit) */
#define PTE_NX           (1ULL << 63)

#define PTE_ADDR_MASK    0x000FFFFFFFFFF000ULL

/* Page table index extraction from virtual address */
#define PML4_INDEX(va)   (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)   (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)     (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)     (((va) >> 12) & 0x1FF)

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
uint64_t  vmm_clone_cow(uint64_t parent_cr3);
void      vmm_free_user_pages(uint64_t cr3);

#endif
