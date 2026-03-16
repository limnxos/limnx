#define pr_fmt(fmt) "[elf] " fmt
#include "klog.h"

#include "proc/elf.h"
#include "mm/vmm.h"
#include "kutil.h"
#include "kquiet.h"
#include "mm/pmm.h"
#include "arch/serial.h"
#include "syscall/syscall.h"  /* USER_ADDR_MAX */
#include "errno.h"

int elf_load(const uint8_t *data, uint64_t size, elf_load_result_t *result) {
    if (size < sizeof(elf64_ehdr_t)) {
        pr_err("file too small for ELF header\n");
        return -EINVAL;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;

    /* Validate magic */
    if (ehdr->e_magic != ELF_MAGIC) {
        pr_err("bad ELF magic\n");
        return -EINVAL;
    }

    /* Validate class, data encoding, type, machine */
    if (ehdr->e_class != ELFCLASS64) {
        pr_err("not 64-bit ELF\n");
        return -EINVAL;
    }
    if (ehdr->e_data != ELFDATA2LSB) {
        pr_err("not little-endian\n");
        return -EINVAL;
    }
    if (ehdr->e_type != ET_EXEC) {
        pr_err("not ET_EXEC\n");
        return -EINVAL;
    }
#if defined(__x86_64__)
    if (ehdr->e_machine != EM_X86_64) {
        pr_err("not x86-64\n");
        return -EINVAL;
    }
#elif defined(__aarch64__)
    if (ehdr->e_machine != EM_AARCH64) {
        pr_err("not aarch64\n");
        return -EINVAL;
    }
#endif

    /* Validate program header table */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        pr_err("no program headers\n");
        return -EINVAL;
    }
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size) {
        pr_err("program headers out of bounds\n");
        return -EINVAL;
    }

    /* Create new address space */
    uint64_t cr3 = vmm_create_address_space();
    if (cr3 == 0) {
        pr_err("failed to create address space\n");
        return -ENOMEM;
    }

    int load_count = 0;
    uint64_t highest_addr = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Validate segment addresses */
        if (phdr->p_vaddr >= USER_ADDR_MAX ||
            phdr->p_vaddr + phdr->p_memsz > USER_ADDR_MAX) {
            pr_err("segment vaddr out of user range\n");
            return -EINVAL;
        }

        /* Validate file bounds */
        if (phdr->p_filesz > 0 &&
            phdr->p_offset + phdr->p_filesz > size) {
            pr_err("segment data out of file bounds\n");
            return -EINVAL;
        }

        if (phdr->p_memsz < phdr->p_filesz) {
            pr_err("memsz < filesz\n");
            return -EINVAL;
        }

        /* Determine PTE flags from ELF p_flags */
        uint64_t pte_flags = PTE_USER;
        if (phdr->p_flags & PF_W)
            pte_flags |= PTE_WRITABLE;
        if (!(phdr->p_flags & PF_X))
            pte_flags |= PTE_NX;

        /* Map pages for [p_vaddr, p_vaddr + p_memsz), page-aligned */
        uint64_t seg_start = phdr->p_vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t seg_end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1)
                           & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t virt = seg_start; virt < seg_end; virt += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (phys == 0) {
                pr_err("out of memory for segment\n");
                return -ENOMEM;
            }

            /* Zero the entire page first */
            uint8_t *page = (uint8_t *)PHYS_TO_VIRT(phys);
            mem_zero(page, PAGE_SIZE);

            /* Copy file data that falls within this page */
            uint64_t page_start = virt;
            uint64_t page_end = virt + PAGE_SIZE;

            uint64_t data_start = phdr->p_vaddr;
            uint64_t data_end = phdr->p_vaddr + phdr->p_filesz;

            /* Overlap between [page_start, page_end) and [data_start, data_end) */
            if (data_start < page_end && data_end > page_start) {
                uint64_t copy_start = data_start > page_start ? data_start : page_start;
                uint64_t copy_end = data_end < page_end ? data_end : page_end;
                uint64_t copy_len = copy_end - copy_start;

                uint64_t page_offset = copy_start - page_start;
                uint64_t file_offset = phdr->p_offset + (copy_start - phdr->p_vaddr);

                for (uint64_t j = 0; j < copy_len; j++)
                    page[page_offset + j] = data[file_offset + j];
            }

            if (vmm_map_page_in(cr3, virt, phys, pte_flags) != 0) {
                pr_err("failed to map segment page\n");
                return -ENOMEM;
            }
        }

        /* Track highest mapped address for brk base */
        uint64_t seg_top = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1)
                           & ~(uint64_t)(PAGE_SIZE - 1);
        if (seg_top > highest_addr)
            highest_addr = seg_top;

        load_count++;
    }

    if (load_count == 0) {
        pr_err("no PT_LOAD segments\n");
        return -EINVAL;
    }

    if (!kernel_quiet)
        pr_info("Valid ELF64: entry=%lx, %d PT_LOAD segments\n",
            ehdr->e_entry, load_count);

    result->entry = ehdr->e_entry;
    result->cr3 = cr3;
    result->brk_base = highest_addr;
    return 0;
}
