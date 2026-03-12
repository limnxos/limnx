#include "syscall/syscall_internal.h"
#include "sched/thread.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/swap.h"
#include "ipc/agent_ns.h"
#include "arch/serial.h"
#include "arch/timer.h"
#include "arch/x86_64/idt.h"

#include "arch/paging.h"

static inline void flush_page(uint64_t addr) {
    arch_flush_tlb_page(addr);
}

int64_t sys_mmap(uint64_t num_pages, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    /* Check memory rlimit */
    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* Allocate contiguous physical pages */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0)
        return -1;

    /* Map pages into process address space */
    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;

        /* Zero the page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;

        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            /* Cleanup: free all allocated pages */
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -1;
        }
    }

    /* Record the mapping */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;

    /* Bump next address (+1 guard gap page) */
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;
    proc->used_mem_pages += num_pages;

    return (int64_t)virt;
}

int64_t sys_munmap(uint64_t virt_addr, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    /* Find the mmap entry */
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (proc->mmap_table[i].used &&
            proc->mmap_table[i].virt_addr == virt_addr) {
            uint32_t npages = proc->mmap_table[i].num_pages;
            uint64_t phys = proc->mmap_table[i].phys_addr;

            if (proc->mmap_table[i].shm_id >= 0) {
                /* Shared memory: clear PTEs + flush TLB, then decrement ref */
                for (uint32_t j = 0; j < npages; j++) {
                    uint64_t va = virt_addr + (uint64_t)j * PAGE_SIZE;
                    uint64_t *pte = vmm_get_pte(proc->cr3, va);
                    if (pte && (*pte & PTE_PRESENT)) {
                        *pte = 0;
                        flush_page(va);
                    }
                }
                int32_t sid = proc->mmap_table[i].shm_id;
                uint64_t sflags;
                shm_lock_acquire(&sflags);
                if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0) {
                    shm_table[sid].ref_count--;
                    if (shm_table[sid].ref_count == 0) {
                        for (uint32_t pi = 0; pi < shm_table[sid].num_pages; pi++) {
                            if (shm_table[sid].phys_pages[pi]) {
                                pmm_free_page(shm_table[sid].phys_pages[pi]);
                                shm_table[sid].phys_pages[pi] = 0;
                            }
                        }
                        shm_table[sid].key = -1;
                        shm_table[sid].num_pages = 0;
                    }
                }
                shm_unlock_release(sflags);
            } else {
                /* Private mapping: clear PTEs, flush TLB, free physical pages */
                for (uint32_t j = 0; j < npages; j++) {
                    uint64_t va = virt_addr + (uint64_t)j * PAGE_SIZE;
                    uint64_t *pte = vmm_get_pte(proc->cr3, va);
                    if (pte && (*pte & PTE_PRESENT)) {
                        uint64_t page_phys = *pte & PTE_ADDR_MASK;
                        *pte = 0;
                        flush_page(va);
                        pmm_free_page(page_phys);
                    } else {
                        /* Fallback: page not in PTE (demand-paged, never faulted) */
                        if (phys)
                            pmm_free_page(phys + (uint64_t)j * PAGE_SIZE);
                    }
                }
            }

            proc->mmap_table[i].used = 0;
            proc->mmap_table[i].shm_id = -1;
            if (proc->used_mem_pages >= npages)
                proc->used_mem_pages -= npages;
            return 0;
        }
    }

    return -1;
}

int64_t sys_fmmap(uint64_t fd, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (fd >= MAX_FDS)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc)
        return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL)
        return -1;

    vfs_node_t *node = entry->node;
    uint64_t file_size = node->size;
    if (file_size == 0)
        return -1;

    /* Allocate mmap pages */
    uint64_t num_pages = (file_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages > MMAP_MAX_PAGES)
        return -1;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;

    /* Allocate contiguous physical pages */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0)
        return -1;

    /* Map pages into process address space and copy file data */
    int node_idx = vfs_node_index(node);
    if (node_idx < 0) {
        for (uint64_t k = 0; k < num_pages; k++)
            pmm_free_page(phys + k * PAGE_SIZE);
        return -1;
    }

    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;

        /* Copy file data into physical page via HHDM */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        uint64_t offset = i * PAGE_SIZE;
        uint64_t chunk = file_size - offset;
        if (chunk > PAGE_SIZE) chunk = PAGE_SIZE;

        /* Read through VFS (handles both RAM and disk-backed files) */
        int64_t got = vfs_read(node_idx, offset, dst, chunk);
        if (got < 0) got = 0;
        for (uint64_t j = (uint64_t)got; j < PAGE_SIZE; j++)
            dst[j] = 0;

        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -1;
        }
    }

    /* Record the mapping */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;

    return (int64_t)virt;
}

int64_t sys_shmget(uint64_t key, uint64_t num_pages,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > 16)
        return -1;

    uint64_t sflags;
    shm_lock_acquire(&sflags);

    /* Search for existing key match */
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (shm_table[i].key == (int32_t)key) {
            shm_unlock_release(sflags);
            return i;
        }
    }

    /* Allocate new region */
    int slot = -1;
    for (int i = 0; i < MAX_SHM_REGIONS; i++) {
        if (shm_table[i].key == -1) { slot = i; break; }
    }
    if (slot < 0) { shm_unlock_release(sflags); return -1; }

    /* Mark slot as taken before releasing lock for page allocation */
    shm_table[slot].key = (int32_t)key;
    shm_table[slot].num_pages = (uint32_t)num_pages;
    shm_table[slot].ref_count = 0;
    shm_unlock_release(sflags);

    /* Allocate physical pages individually (outside lock — pmm has its own) */
    for (uint32_t i = 0; i < (uint32_t)num_pages; i++) {
        uint64_t pg = pmm_alloc_page();
        if (pg == 0) {
            /* Free already allocated */
            for (uint32_t j = 0; j < i; j++)
                pmm_free_page(shm_table[slot].phys_pages[j]);
            shm_lock_acquire(&sflags);
            shm_table[slot].key = -1;
            shm_unlock_release(sflags);
            return -1;
        }
        /* Zero the page */
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(pg);
        for (uint64_t j = 0; j < PAGE_SIZE; j++)
            dst[j] = 0;
        shm_table[slot].phys_pages[i] = pg;
    }

    return slot;
}

int64_t sys_shmat(uint64_t shmid, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    uint64_t sflags;
    shm_lock_acquire(&sflags);
    if (shmid >= MAX_SHM_REGIONS || shm_table[shmid].key == -1) {
        shm_unlock_release(sflags);
        return 0;
    }

    uint32_t npages = shm_table[shmid].num_pages;
    uint64_t phys_pages_copy[16];
    for (uint32_t i = 0; i < npages; i++)
        phys_pages_copy[i] = shm_table[shmid].phys_pages[i];
    shm_unlock_release(sflags);

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return 0;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0;

    uint64_t virt = proc->mmap_next_addr;

    /* Map each page individually and increment refcount for PTE reference */
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t page_phys = phys_pages_copy[i];
        uint64_t page_virt = virt + (uint64_t)i * PAGE_SIZE;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0)
            return 0;
        pmm_ref_inc(page_phys);
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys_pages_copy[0];
    proc->mmap_table[slot].num_pages = npages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = (int32_t)shmid;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (uint64_t)(npages + 1) * PAGE_SIZE;

    shm_lock_acquire(&sflags);
    shm_table[shmid].ref_count++;
    shm_unlock_release(sflags);

    return (int64_t)virt;
}

int64_t sys_shmdt(uint64_t virt_addr, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (proc->mmap_table[i].used &&
            proc->mmap_table[i].virt_addr == virt_addr &&
            proc->mmap_table[i].shm_id >= 0) {
            int32_t sid = proc->mmap_table[i].shm_id;
            uint32_t npages = proc->mmap_table[i].num_pages;
            {
                uint64_t sflags;
                shm_lock_acquire(&sflags);
                if (sid < MAX_SHM_REGIONS && shm_table[sid].ref_count > 0) {
                    shm_table[sid].ref_count--;
                    if (shm_table[sid].ref_count == 0) {
                        for (uint32_t pi = 0; pi < shm_table[sid].num_pages; pi++) {
                            if (shm_table[sid].phys_pages[pi]) {
                                pmm_free_page(shm_table[sid].phys_pages[pi]);
                                shm_table[sid].phys_pages[pi] = 0;
                            }
                        }
                        shm_table[sid].key = -1;
                        shm_table[sid].num_pages = 0;
                    }
                }
                shm_unlock_release(sflags);
            }
            /* Unmap pages from page table, flush TLB, drop PTE refcount */
            for (uint32_t p = 0; p < npages; p++) {
                uint64_t pv = virt_addr + (uint64_t)p * PAGE_SIZE;
                uint64_t *pte = vmm_get_pte(proc->cr3, pv);
                if (pte && (*pte & PTE_PRESENT)) {
                    uint64_t phys = *pte & PTE_ADDR_MASK;
                    *pte = 0;
                    flush_page(pv);
                    pmm_free_page(phys);
                }
            }
            proc->mmap_table[i].used = 0;
            proc->mmap_table[i].shm_id = -1;
            return 0;
        }
    }
    return -1;
}

int64_t sys_mmap2(uint64_t num_pages, uint64_t mmap_flags,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    /* Check memory rlimit */
    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    /* Namespace memory quota check */
    if (!agent_ns_quota_check(proc->ns_id, NS_QUOTA_MEM_PAGES, (uint32_t)num_pages))
        return -ENOMEM;

    /* Find a free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -ENOMEM;

    uint64_t virt = proc->mmap_next_addr;

    if (mmap_flags & MMAP_DEMAND) {
        /* Demand-paged: reserve address space but don't allocate physical pages */
        proc->mmap_table[slot].virt_addr = virt;
        proc->mmap_table[slot].phys_addr = 0;
        proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
        proc->mmap_table[slot].used = 1;
        proc->mmap_table[slot].shm_id = -1;
        proc->mmap_table[slot].demand = 1;
        proc->mmap_table[slot].vfs_node = -1;
        proc->mmap_table[slot].file_offset = 0;
        proc->mmap_next_addr = virt + (num_pages + 1) * 4096;
        proc->used_mem_pages += num_pages;
        return (int64_t)virt;
    }

    /* Eager allocation — same as sys_mmap */
    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0) return -ENOMEM;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * 4096;
        uint64_t page_virt = virt + i * 4096;
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (int j = 0; j < 4096; j++) dst[j] = 0;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0)
            return -ENOMEM;
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 0;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    proc->mmap_next_addr = virt + (num_pages + 1) * 4096;
    proc->used_mem_pages += num_pages;
    return (int64_t)virt;
}

int64_t sys_mmap_file(uint64_t fd, uint64_t offset,
                              uint64_t num_pages, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;
    if (fd >= MAX_FDS || num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -1;

    thread_t *t = thread_get_current();
    process_t *proc = t->process;
    if (!proc) return -1;

    fd_entry_t *entry = &proc->fd_table[fd];
    if (entry->node == NULL) return -1;

    vfs_node_t *node = entry->node;
    int node_idx = vfs_node_index(node);
    if (node_idx < 0) return -1;

    /* Find free mmap slot */
    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return -12;  /* -ENOMEM */

    uint64_t virt = proc->mmap_next_addr;

    /* Reserve virtual address space — no physical pages allocated */
    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = 0;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 1;
    proc->mmap_table[slot].vfs_node = (int32_t)node_idx;
    proc->mmap_table[slot].file_offset = offset;
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;

    return (int64_t)virt;
}

int64_t sys_mprotect(uint64_t virt_addr, uint64_t num_pages,
                             uint64_t prot, uint64_t a4, uint64_t a5) {
    (void)a4; (void)a5;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -EINVAL;

    if (virt_addr & 0xFFF) return -EINVAL;  /* not page-aligned */
    if (num_pages == 0) return -EINVAL;

    uint64_t range_end = virt_addr + num_pages * PAGE_SIZE;

    /* Validate: entire range must be within a single mmap entry */
    int found = 0;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) continue;
        uint64_t entry_start = proc->mmap_table[i].virt_addr;
        uint64_t entry_end = entry_start + (uint64_t)proc->mmap_table[i].num_pages * PAGE_SIZE;
        if (virt_addr >= entry_start && range_end <= entry_end) {
            found = 1;
            break;
        }
    }
    if (!found) return -EINVAL;

    /* Convert prot flags to PTE flags */
    uint64_t new_flags = PTE_PRESENT | PTE_USER;
    if (prot == PROT_NONE) {
        new_flags = PTE_USER;  /* no PTE_PRESENT — page inaccessible */
    } else {
        if (prot & PROT_WRITE)
            new_flags |= PTE_WRITABLE;
        if (!(prot & PROT_EXEC))
            new_flags |= PTE_NX;
    }

    /* Walk PTEs and update flags */
    for (uint64_t pg = 0; pg < num_pages; pg++) {
        uint64_t va = virt_addr + pg * PAGE_SIZE;
        uint64_t *pte = vmm_get_pte(proc->cr3, va);
        if (!pte) continue;  /* demand page not yet faulted — skip */
        uint64_t old = *pte;
        if (!(old & PTE_PRESENT) && !(old & PTE_SWAP)) continue;  /* not mapped yet */

        uint64_t phys = old & PTE_ADDR_MASK;
        /* Preserve COW bit if set (don't grant write on COW page via mprotect) */
        uint64_t cow = old & PTE_COW;
        uint64_t final_flags = new_flags | cow;
        if (cow) {
            if (final_flags & PTE_WRITABLE) {
                /* COW page: don't actually make writable yet — keep COW semantics.
                 * But mark WAS_WRITABLE so COW handler knows to grant write later. */
                final_flags &= ~PTE_WRITABLE;
                final_flags |= PTE_WAS_WRITABLE;
            } else {
                /* Making COW page read-only: clear WAS_WRITABLE so COW handler
                 * knows this is a genuine protection fault */
                final_flags &= ~PTE_WAS_WRITABLE;
            }
        }

        *pte = phys | final_flags;
        flush_page(va);
    }

    return 0;
}

int64_t sys_mmap_guard(uint64_t num_pages, uint64_t a2,
                               uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (num_pages == 0 || num_pages > MMAP_MAX_PAGES)
        return -EINVAL;

    thread_t *t = thread_get_current();
    process_t *proc = t ? t->process : NULL;
    if (!proc) return -EINVAL;

    if (proc->rlimit_mem_pages > 0 &&
        proc->used_mem_pages + num_pages > proc->rlimit_mem_pages)
        return -ENOMEM;

    int slot = -1;
    for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
        if (!proc->mmap_table[i].used) { slot = i; break; }
    }
    if (slot < 0) return -ENOMEM;

    uint64_t phys = pmm_alloc_contiguous((uint32_t)num_pages);
    if (phys == 0) return -ENOMEM;

    uint64_t virt = proc->mmap_next_addr;
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t page_phys = phys + i * PAGE_SIZE;
        uint64_t page_virt = virt + i * PAGE_SIZE;
        uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(page_phys);
        for (uint64_t j = 0; j < PAGE_SIZE; j++) dst[j] = 0;
        if (vmm_map_page_in(proc->cr3, page_virt, page_phys,
                            PTE_USER | PTE_WRITABLE | PTE_NX) != 0) {
            for (uint64_t k = 0; k < num_pages; k++)
                pmm_free_page(phys + k * PAGE_SIZE);
            return -ENOMEM;
        }
    }

    proc->mmap_table[slot].virt_addr = virt;
    proc->mmap_table[slot].phys_addr = phys;
    proc->mmap_table[slot].num_pages = (uint32_t)num_pages;
    proc->mmap_table[slot].used = 1;
    proc->mmap_table[slot].shm_id = -1;
    proc->mmap_table[slot].demand = 0;
    proc->mmap_table[slot].vfs_node = -1;
    proc->mmap_table[slot].file_offset = 0;
    /* Skip 1 guard page after the usable region */
    proc->mmap_next_addr = virt + (num_pages + 1) * PAGE_SIZE;
    proc->used_mem_pages += num_pages;

    return (int64_t)virt;
}

/* --- Page fault handler --- */

int page_fault_handler(uint64_t fault_addr, uint64_t err_code,
                       interrupt_frame_t *frame) {
    thread_t *t = thread_get_current();
    if (!t || !t->process) return -1;  /* Kernel fault */

    process_t *proc = t->process;

    int present = err_code & 1;
    int write   = err_code & 2;
    int user    = err_code & 4;

    if (!user) return -1;  /* Kernel-mode fault */

    if (present && write) {
        /* Page present but not writable — check COW */
        uint64_t *pte = vmm_get_pte(proc->cr3, fault_addr);
        if (pte && (*pte & PTE_COW)) {
            if (!(*pte & PTE_WAS_WRITABLE))
                goto kill;

            uint64_t old_phys = *pte & PTE_ADDR_MASK;
            uint64_t old_flags = *pte & ~PTE_ADDR_MASK;
            uint64_t clean_flags = old_flags & ~(PTE_COW | PTE_WAS_WRITABLE);

            if (pmm_ref_get(old_phys) == 1) {
                *pte = old_phys | (clean_flags | PTE_WRITABLE | PTE_PRESENT);
                invlpg_addr(fault_addr & ~0xFFFULL);
                return 0;
            }

            uint64_t new_phys = pmm_alloc_page();
            if (new_phys == 0) goto kill;

            uint8_t *src = (uint8_t *)PHYS_TO_VIRT(old_phys);
            uint8_t *dst = (uint8_t *)PHYS_TO_VIRT(new_phys);
            for (int i = 0; i < 4096; i++) dst[i] = src[i];

            *pte = new_phys | (clean_flags | PTE_WRITABLE | PTE_PRESENT);
            invlpg_addr(fault_addr & ~0xFFFULL);

            pmm_ref_dec(old_phys);
            return 0;
        }
    }

    /* Demand paging / swap-in: page not present in user mode */
    if (!present && user) {
        uint64_t *pte = vmm_get_pte(proc->cr3, fault_addr);
        if (pte && swap_is_entry(*pte)) {
            if (swap_in(proc->cr3, fault_addr) == 0) {
                invlpg_addr(fault_addr & ~0xFFFULL);
                return 0;
            }
        }

        uint64_t page_addr = fault_addr & ~0xFFFULL;
        for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
            mmap_entry_t *me = &proc->mmap_table[i];
            if (!me->used || !me->demand) continue;
            uint64_t region_start = me->virt_addr;
            uint64_t region_end = region_start + (uint64_t)me->num_pages * 4096;
            if (page_addr >= region_start && page_addr < region_end) {
                if (me->vfs_node >= 0) {
                    uint64_t new_phys = pmm_alloc_page();
                    if (new_phys == 0) goto kill;
                    uint8_t *fdst = (uint8_t *)PHYS_TO_VIRT(new_phys);

                    uint64_t page_offset_in_region = page_addr - region_start;
                    uint64_t file_off = me->file_offset + page_offset_in_region;
                    int64_t got = vfs_read(me->vfs_node, file_off, fdst, 4096);
                    if (got < 0) got = 0;
                    for (int64_t j = got; j < 4096; j++)
                        fdst[j] = 0;

                    if (vmm_map_page_in(proc->cr3, page_addr, new_phys,
                                        PTE_USER | PTE_NX) == 0) {
                        invlpg_addr(page_addr);
                        return 0;
                    }
                    pmm_free_page(new_phys);
                } else if (demand_page_fault(proc->cr3, page_addr) == 0) {
                    invlpg_addr(page_addr);
                    return 0;
                }
            }
        }
    }

kill:
    {
        uint64_t stack_bottom = USER_STACK_TOP - USER_STACK_SIZE;
        uint64_t guard_start = stack_bottom - PAGE_SIZE;
        if (fault_addr >= guard_start && fault_addr < stack_bottom) {
            serial_printf("[fault] Stack overflow detected (pid %lu, addr=%lx, rip=%lx)\n",
                proc->pid, fault_addr, frame->rip);
        } else {
            int in_guard_gap = 0;
            for (int i = 0; i < MMAP_MAX_ENTRIES; i++) {
                mmap_entry_t *me = &proc->mmap_table[i];
                if (!me->used) continue;
                uint64_t region_end = me->virt_addr + (uint64_t)me->num_pages * PAGE_SIZE;
                uint64_t gap_end = region_end + PAGE_SIZE;
                if (fault_addr >= region_end && fault_addr < gap_end) {
                    serial_printf("[fault] Guard page hit (pid %lu, addr=%lx past mmap %lx+%u, rip=%lx)\n",
                        proc->pid, fault_addr, me->virt_addr, me->num_pages, frame->rip);
                    in_guard_gap = 1;
                    break;
                }
            }
            if (!in_guard_gap)
                serial_printf("[fault] Process %lu killed: fault at %lx (err=%lx, rip=%lx)\n",
                    proc->pid, fault_addr, err_code, frame->rip);
        }
    }
    process_terminate(t, -11);
    return 0;
}
