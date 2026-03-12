#ifndef LIMNX_ARCH_PAGING_H
#define LIMNX_ARCH_PAGING_H

/*
 * Architecture-independent paging interface.
 *
 * Each architecture provides:
 *   arch_switch_address_space(phys) — switch to a different page table root
 *   arch_get_address_space()        — get current page table root physical addr
 *   arch_flush_tlb_page(virt)       — invalidate TLB entry for a single page
 *   arch_get_fault_address()        — get faulting virtual address
 */

#if defined(__x86_64__)
#include "arch/x86_64/paging.h"
#elif defined(__aarch64__)
#include "arch/arm64/paging.h"
#else
#error "Unsupported architecture"
#endif

#endif /* LIMNX_ARCH_PAGING_H */
