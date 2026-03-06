#include "proc/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "serial.h"
#include "syscall/syscall.h"  /* USER_ADDR_MAX */

int elf_load(const uint8_t *data, uint64_t size, elf_load_result_t *result) {
    if (size < sizeof(elf64_ehdr_t)) {
        serial_puts("[elf] ERROR: file too small for ELF header\n");
        return -1;
    }

    const elf64_ehdr_t *ehdr = (const elf64_ehdr_t *)data;

    /* Validate magic */
    if (ehdr->e_magic != ELF_MAGIC) {
        serial_puts("[elf] ERROR: bad ELF magic\n");
        return -1;
    }

    /* Validate class, data encoding, type, machine */
    if (ehdr->e_class != ELFCLASS64) {
        serial_puts("[elf] ERROR: not 64-bit ELF\n");
        return -1;
    }
    if (ehdr->e_data != ELFDATA2LSB) {
        serial_puts("[elf] ERROR: not little-endian\n");
        return -1;
    }
    if (ehdr->e_type != ET_EXEC) {
        serial_puts("[elf] ERROR: not ET_EXEC\n");
        return -1;
    }
    if (ehdr->e_machine != EM_X86_64) {
        serial_puts("[elf] ERROR: not x86-64\n");
        return -1;
    }

    /* Validate program header table */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        serial_puts("[elf] ERROR: no program headers\n");
        return -1;
    }
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size) {
        serial_puts("[elf] ERROR: program headers out of bounds\n");
        return -1;
    }

    /* Create new address space */
    uint64_t cr3 = vmm_create_address_space();
    if (cr3 == 0) {
        serial_puts("[elf] ERROR: failed to create address space\n");
        return -1;
    }

    int load_count = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr = (const elf64_phdr_t *)
            (data + ehdr->e_phoff + (uint64_t)i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD)
            continue;

        /* Validate segment addresses */
        if (phdr->p_vaddr >= USER_ADDR_MAX ||
            phdr->p_vaddr + phdr->p_memsz > USER_ADDR_MAX) {
            serial_puts("[elf] ERROR: segment vaddr out of user range\n");
            return -1;
        }

        /* Validate file bounds */
        if (phdr->p_filesz > 0 &&
            phdr->p_offset + phdr->p_filesz > size) {
            serial_puts("[elf] ERROR: segment data out of file bounds\n");
            return -1;
        }

        if (phdr->p_memsz < phdr->p_filesz) {
            serial_puts("[elf] ERROR: memsz < filesz\n");
            return -1;
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
                serial_puts("[elf] ERROR: out of memory for segment\n");
                return -1;
            }

            /* Zero the entire page first */
            uint8_t *page = (uint8_t *)PHYS_TO_VIRT(phys);
            for (uint64_t j = 0; j < PAGE_SIZE; j++)
                page[j] = 0;

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
                serial_puts("[elf] ERROR: failed to map segment page\n");
                return -1;
            }
        }

        load_count++;
    }

    if (load_count == 0) {
        serial_puts("[elf] ERROR: no PT_LOAD segments\n");
        return -1;
    }

    serial_printf("[elf] Valid ELF64: entry=%lx, %d PT_LOAD segments\n",
        ehdr->e_entry, load_count);

    result->entry = ehdr->e_entry;
    result->cr3 = cr3;
    return 0;
}
