#ifndef LIMNX_ELF_H
#define LIMNX_ELF_H

#include <stdint.h>

/* ELF magic: "\x7fELF" as uint32_t (little-endian) */
#define ELF_MAGIC 0x464C457F

/* e_class */
#define ELFCLASS64 2

/* e_data */
#define ELFDATA2LSB 1

/* e_type */
#define ET_EXEC 2

/* e_machine */
#define EM_X86_64 0x3E

/* p_type */
#define PT_LOAD 1

/* p_flags */
#define PF_X 1
#define PF_W 2
#define PF_R 4

typedef struct {
    uint32_t e_magic;
    uint8_t  e_class;       /* 2 = 64-bit */
    uint8_t  e_data;        /* 1 = little-endian */
    uint8_t  e_version;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;        /* 2 = ET_EXEC */
    uint16_t e_machine;     /* 0x3E = x86-64 */
    uint32_t e_version2;
    uint64_t e_entry;       /* entry point virtual address */
    uint64_t e_phoff;       /* program header table offset */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_flags;       /* PF_X=1, PF_W=2, PF_R=4 */
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;       /* >= filesz, difference is BSS */
    uint64_t p_align;
} elf64_phdr_t;

typedef struct {
    uint64_t entry;         /* from e_entry */
    uint64_t cr3;           /* new address space with segments mapped */
} elf_load_result_t;

/*
 * Load an ELF64 executable into a new address space.
 * Returns 0 on success, -1 on failure.
 */
int elf_load(const uint8_t *data, uint64_t size, elf_load_result_t *result);

#endif
