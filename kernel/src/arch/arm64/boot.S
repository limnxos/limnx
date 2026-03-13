/*
 * ARM64 kernel entry point for QEMU virt machine.
 * Loaded by QEMU -kernel at the address specified in the ELF header.
 * Runs at EL1 (QEMU -kernel mode drops to EL1 before entry).
 */

.section .text.boot
.global _start
_start:
    /* Disable interrupts */
    msr     daifset, #0xf

    /* Read CPU ID from MPIDR_EL1 — only boot on CPU 0 */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbnz    x0, .Lhalt

    /* Clear alignment check bit in SCTLR_EL1 so unaligned NEON accesses work */
    mrs     x0, sctlr_el1
    bic     x0, x0, #(1 << 1)       /* Clear A (alignment check) bit */
    msr     sctlr_el1, x0
    isb

    /* Set up the stack (grows down from _stack_top) */
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0

    /* Zero BSS */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
.Lbss_loop:
    cmp     x0, x1
    b.ge    .Lbss_done
    str     xzr, [x0], #8
    b       .Lbss_loop
.Lbss_done:

    /* Call kmain */
    bl      kmain

    /* If kmain returns, halt */
.Lhalt:
    wfi
    b       .Lhalt

/* Kernel stack — 64KB */
.section .bss
.align 16
_stack_bottom:
    .space 65536
.global _stack_top
_stack_top:
