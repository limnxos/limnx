#define pr_fmt(fmt) "[dma]  " fmt
#include "klog.h"

#include "mm/dma.h"
#include "mm/pmm.h"
#include "kutil.h"

void *dma_alloc(size_t size) {
    if (size == 0)
        return NULL;

    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys;

    if (pages == 1)
        phys = pmm_alloc_page();
    else
        phys = pmm_alloc_contiguous(pages);

    if (phys == 0) {
        pr_err("failed to alloc %u pages\n", pages);
        return NULL;
    }

    void *virt = (void *)PHYS_TO_VIRT(phys);
    mem_zero(virt, (uint64_t)pages * PAGE_SIZE);

    return virt;
}

void dma_free(void *vaddr, size_t size) {
    if (!vaddr || size == 0)
        return;

    uint64_t phys = VIRT_TO_PHYS(vaddr);
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < pages; i++)
        pmm_free_page(phys + i * PAGE_SIZE);
}

uint64_t dma_virt_to_phys(void *vaddr) {
    return VIRT_TO_PHYS(vaddr);
}
