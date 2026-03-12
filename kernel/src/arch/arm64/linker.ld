/*
 * ARM64 kernel linker script for QEMU virt machine.
 * QEMU -kernel loads ELF at the entry point address.
 * We place the kernel at 0x40080000 (just above QEMU's DTB area).
 */

ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

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
