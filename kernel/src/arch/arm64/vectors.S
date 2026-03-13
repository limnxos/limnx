/*
 * ARM64 exception vector table
 *
 * VBAR_EL1 points to a 2KB-aligned table with 16 entries (4 groups x 4 types).
 * Each entry is 128 bytes (32 instructions max).
 *
 * Since SAVE_CONTEXT + RESTORE_CONTEXT exceed 128 bytes, each vector entry
 * is a trampoline that branches to handler code outside the table.
 */

.section .text

.macro SAVE_CONTEXT
    sub     sp, sp, #272            /* 34 x 8 bytes */
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    mrs     x23, sp_el0
    stp     x30, x21, [sp, #240]
    stp     x22, x23, [sp, #256]
.endm

.macro RESTORE_CONTEXT
    ldp     x22, x23, [sp, #256]
    ldp     x30, x21, [sp, #240]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    msr     sp_el0, x23
    ldp     x28, x29, [sp, #224]
    ldp     x26, x27, [sp, #208]
    ldp     x24, x25, [sp, #192]
    ldp     x22, x23, [sp, #176]
    ldp     x20, x21, [sp, #160]
    ldp     x18, x19, [sp, #144]
    ldp     x16, x17, [sp, #128]
    ldp     x14, x15, [sp, #112]
    ldp     x12, x13, [sp, #96]
    ldp     x10, x11, [sp, #80]
    ldp     x8,  x9,  [sp, #64]
    ldp     x6,  x7,  [sp, #48]
    ldp     x4,  x5,  [sp, #32]
    ldp     x2,  x3,  [sp, #16]
    ldp     x0,  x1,  [sp, #0]
    add     sp, sp, #272
.endm

/*
 * Exception vector table — must be 2KB aligned.
 * Each entry is a branch trampoline (128 bytes, only uses 4 bytes).
 */
.align 11
.global arm64_vectors
arm64_vectors:

/* Group 0: Current EL with SP_EL0 (unused) */
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled

/* Group 1: Current EL with SP_ELx (kernel) */
.align 7
    b       handler_celx_sync
.align 7
    b       handler_celx_irq
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled

/* Group 2: Lower EL, AArch64 (user-space) */
.align 7
    b       handler_lower64_sync
.align 7
    b       handler_lower64_irq
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled

/* Group 3: Lower EL, AArch32 (not supported) */
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled
.align 7
    b       handler_unhandled

/*
 * Handler implementations (outside the vector table)
 */

handler_celx_sync:
    SAVE_CONTEXT
    mov     x0, sp
    mrs     x1, esr_el1
    bl      arm64_sync_handler
    RESTORE_CONTEXT
    eret

handler_celx_irq:
    SAVE_CONTEXT
    mov     x0, sp
    bl      arm64_irq_handler
    RESTORE_CONTEXT
    eret

handler_lower64_sync:
    SAVE_CONTEXT
    mov     x0, sp
    mrs     x1, esr_el1
    bl      arm64_sync_handler
    RESTORE_CONTEXT
    eret

handler_lower64_irq:
    SAVE_CONTEXT
    mov     x0, sp
    bl      arm64_irq_handler
    RESTORE_CONTEXT
    eret

handler_unhandled:
    wfi
    b       handler_unhandled
