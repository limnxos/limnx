#ifndef LIMNX_ARM64_PAGING_H
#define LIMNX_ARM64_PAGING_H

#include <stdint.h>

static inline void arch_switch_address_space(uint64_t phys) {
    /* Write to TTBR0_EL1 (user-space page tables) */
    __asm__ volatile ("msr ttbr0_el1, %0" : : "r"(phys));
    /* Invalidate entire TLB for EL0/EL1 — required after TTBR switch */
    __asm__ volatile ("tlbi vmalle1is");
    __asm__ volatile ("dsb sy");
    __asm__ volatile ("isb");
}

static inline uint64_t arch_get_address_space(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, ttbr0_el1" : "=r"(val));
    return val;
}

static inline void arch_flush_tlb_page(uint64_t virt_addr) {
    /* TLBI VAE1IS — invalidate by VA, EL1, inner-shareable */
    uint64_t page = virt_addr >> 12;
    __asm__ volatile ("tlbi vae1is, %0" : : "r"(page));
    __asm__ volatile ("dsb sy");
    __asm__ volatile ("isb");
}

static inline uint64_t arch_get_fault_address(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, far_el1" : "=r"(val));
    return val;
}

#endif /* LIMNX_ARM64_PAGING_H */
