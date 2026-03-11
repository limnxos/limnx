#ifndef LIMNX_PMM_H
#define LIMNX_PMM_H

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

#define PAGE_SIZE 4096

#define PHYS_TO_VIRT(phys) ((void *)((uint64_t)(phys) + hhdm_offset))
#define VIRT_TO_PHYS(virt) ((uint64_t)(virt) - hhdm_offset)

extern uint64_t hhdm_offset;

void     pmm_init(void);
__must_check uint64_t pmm_alloc_page(void);
__must_check uint64_t pmm_alloc_contiguous(uint64_t count);
void     pmm_free_page(uint64_t phys_addr);
uint64_t pmm_get_total_pages(void);
uint64_t pmm_get_free_pages(void);

/* Reference counting for COW */
void     pmm_ref_inc(uint64_t phys_addr);
int      pmm_ref_dec(uint64_t phys_addr);
uint16_t pmm_ref_get(uint64_t phys_addr);

#endif
