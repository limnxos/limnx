#ifndef LIMNX_DMA_H
#define LIMNX_DMA_H

/*
 * DMA-coherent memory allocator.
 *
 * Provides physically contiguous, zero-initialized memory for device DMA.
 * On QEMU (x86_64 and ARM64 virt), DMA is cache-coherent via the
 * platform interconnect, so normal memory suffices.  For real hardware
 * without HW coherency, dma_alloc would need non-cacheable mappings.
 */

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

/* Allocate page-aligned, physically contiguous, zeroed DMA memory.
 * Returns virtual address or NULL on failure. */
__must_check void *dma_alloc(size_t size);

/* Free DMA memory previously returned by dma_alloc. */
void dma_free(void *vaddr, size_t size);

/* Convert DMA virtual address to physical address for descriptor rings. */
uint64_t dma_virt_to_phys(void *vaddr);

#endif
