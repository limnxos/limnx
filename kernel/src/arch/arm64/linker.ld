/*
 * ARM64 kernel linker script for QEMU virt machine.
 * QEMU -kernel loads ELF at the entry point address.
 * We place the kernel at 0x40200000 (above QEMU's DTB area, ~2MB gap).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40200000;

    .text : {
        *(.text.boot)      /* Entry point first */
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    __bss_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
