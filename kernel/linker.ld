/* Limnx Kernel Linker Script — x86_64, Limine boot protocol */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kmain)

PHDRS {
    requests PT_LOAD FLAGS((1 << 1) | (1 << 0));          /* RW */
    text     PT_LOAD FLAGS((1 << 2) | (1 << 0));          /* RX */
    rodata   PT_LOAD FLAGS((1 << 0));                      /* R  */
    data     PT_LOAD FLAGS((1 << 1) | (1 << 0));          /* RW */
}

SECTIONS {
    . = 0xffffffff80000000;

    __kernel_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(4096);
    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    } :data

    . = ALIGN(4096);
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    . = ALIGN(4096);
    __kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
