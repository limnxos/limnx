#ifndef LIMNX_X86_64_PTE_H
#define LIMNX_X86_64_PTE_H

/*
 * x86_64 page table entry bit definitions.
 * 4-level paging (PML4 → PDPT → PD → PT), 4KB pages.
 */

#include <stdint.h>

/* Hardware PTE flags */
#define PTE_PRESENT      (1ULL << 0)
#define PTE_TABLE        (0ULL)          /* x86_64: no table/block distinction */
#define PTE_WRITABLE     (1ULL << 1)
#define PTE_USER         (1ULL << 2)
#define PTE_WRITETHROUGH (1ULL << 3)
#define PTE_NOCACHE      (1ULL << 4)
#define PTE_ACCESSED     (1ULL << 5)
#define PTE_DIRTY        (1ULL << 6)
#define PTE_HUGE         (1ULL << 7)
#define PTE_GLOBAL       (1ULL << 8)
#define PTE_NX           (1ULL << 63)

/* Software-defined bits (available bits 9-11) */
#define PTE_COW          (1ULL << 9)   /* Copy-on-write */
#define PTE_SWAP         (1ULL << 10)  /* Page swapped to disk */
#define PTE_WAS_WRITABLE (1ULL << 11)  /* Page was writable before COW */

/* ARM64 memory attribute compatibility (no-op on x86_64) */
#define PTE_ATTRINDX_NORMAL  (0ULL)
#define PTE_ATTRINDX_DEVICE  (0ULL)
#define PTE_SH_ISH           (0ULL)

/* Physical address mask (bits 12-51) */
#define PTE_ADDR_MASK    0x000FFFFFFFFFF000ULL

/* Page table index extraction from virtual address */
#define PML4_INDEX(va)   (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)   (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)     (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)     (((va) >> 12) & 0x1FF)

#endif
