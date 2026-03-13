#ifndef LIMNX_ARM64_PTE_H
#define LIMNX_ARM64_PTE_H

/*
 * ARM64 VMSAv8-A Stage 1 page table descriptor bits.
 * 4KB granule, 4-level (L0-L3) translation tables.
 *
 * These map to the same macro names as x86_64 where possible,
 * allowing shared VMM code to use them generically.
 */

#include <stdint.h>

/* Descriptor valid bit */
#define PTE_PRESENT      (1ULL << 0)

/* Table descriptor type bit (bit 1 = 1 for table, 0 for block) */
#define PTE_TABLE        (1ULL << 1)

/* Access permissions:
 * AP[2:1] in bits [7:6]
 * AP[2]=0 → RW, AP[2]=1 → RO
 * AP[1]=1 → EL0 accessible
 */
#define PTE_WRITABLE     (0ULL)        /* AP[2]=0 means RW */
#define PTE_USER         (1ULL << 6)   /* AP[1]=1, EL0 access */
#define PTE_READONLY     (1ULL << 7)   /* AP[2]=1, read-only */

/* Memory attributes (AttrIndx in bits [4:2]):
 * Maps to MAIR_EL1 indices set in boot.S:
 *   Index 0 = Normal WB (0xFF)
 *   Index 1 = Device-nGnRnE (0x00) */
#define PTE_ATTRINDX_NORMAL  (0ULL << 2)  /* AttrIndx=0: Normal memory */
#define PTE_ATTRINDX_DEVICE  (1ULL << 2)  /* AttrIndx=1: Device memory */
#define PTE_WRITETHROUGH (1ULL << 3)   /* AttrIndx hint (compat) */
#define PTE_NOCACHE      (1ULL << 4)   /* AttrIndx for device memory (compat) */

/* Shareability (bits [9:8]) */
#define PTE_SH_ISH       (3ULL << 8)  /* Inner Shareable */

/* Access flag (bit 10) */
#define PTE_ACCESSED     (1ULL << 10)

/* Dirty bit (DBM, bit 51 if HW dirty management supported) */
#define PTE_DIRTY        (1ULL << 51)

/* Block descriptor (for L1/L2 huge pages) */
#define PTE_HUGE         (0ULL)        /* Block descriptor at L1/L2 */

/* nG bit (bit 11) — not global */
#define PTE_GLOBAL       (0ULL)        /* nG=0 means global */

/* Execute-never (UXN bit 54, PXN bit 53) */
#define PTE_NX           (1ULL << 54)

/* Software-defined bits (available bits) */
#define PTE_COW          (1ULL << 55)
#define PTE_SWAP         (1ULL << 56)
#define PTE_WAS_WRITABLE (1ULL << 57)

/* Physical address mask (bits 12-47 for 48-bit PA) */
#define PTE_ADDR_MASK    0x0000FFFFFFFFF000ULL

/* Page table index extraction (4KB granule, 9 bits per level) */
#define PML4_INDEX(va)   (((va) >> 39) & 0x1FF)  /* L0 */
#define PDPT_INDEX(va)   (((va) >> 30) & 0x1FF)  /* L1 */
#define PD_INDEX(va)     (((va) >> 21) & 0x1FF)  /* L2 */
#define PT_INDEX(va)     (((va) >> 12) & 0x1FF)  /* L3 */

#endif
