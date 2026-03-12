#ifndef LIMNX_X86_64_PAGING_H
#define LIMNX_X86_64_PAGING_H

#include <stdint.h>

static inline void arch_switch_address_space(uint64_t phys) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}

static inline uint64_t arch_get_address_space(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void arch_flush_tlb_page(uint64_t virt_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

static inline uint64_t arch_get_fault_address(void) {
    uint64_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

#endif /* LIMNX_X86_64_PAGING_H */
