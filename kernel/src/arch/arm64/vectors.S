/*
 * ARM64 exception vector table
 *
 * Corresponds to x86_64 isr_stubs.asm.
 *
 * VBAR_EL1 points to a 2KB-aligned table with 16 entries (4 groups x 4 types).
 * Each entry is 128 bytes (32 instructions max).
 *
 * Groups:
 *   0: Current EL, SP_EL0 (not used — kernel uses SP_EL1)
 *   1: Current EL, SP_ELx (kernel exceptions)
 *   2: Lower EL, AArch64 (user-space exceptions/syscalls)
 *   3: Lower EL, AArch32 (not supported)
 */

.section .text

.macro VECTOR_ENTRY label
.align 7    /* 128-byte alignment per vector entry */
\label:
.endm

.macro SAVE_CONTEXT
    /* Save all general-purpose registers */
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
    /* Save x30 (LR), ELR_EL1, SPSR_EL1, SP_EL0 */
    mrs     x21, elr_el1
    mrs     x22, spsr_el1
    mrs     x23, sp_el0
    stp     x30, x21, [sp, #240]
    stp     x22, x23, [sp, #256]
.endm

.macro RESTORE_CONTEXT
    /* Restore ELR_EL1, SPSR_EL1, SP_EL0 */
    ldp     x22, x23, [sp, #256]
    ldp     x30, x21, [sp, #240]
    msr     elr_el1, x21
    msr     spsr_el1, x22
    msr     sp_el0, x23
    /* Restore general-purpose registers */
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
 * Exception vector table — must be 2KB aligned
 */
.align 11
.global arm64_vectors
arm64_vectors:

/* ---- Group 0: Current EL with SP_EL0 (unused) ---- */
VECTOR_ENTRY vec_cel0_sync
    b       vec_unhandled
VECTOR_ENTRY vec_cel0_irq
    b       vec_unhandled
VECTOR_ENTRY vec_cel0_fiq
    b       vec_unhandled
VECTOR_ENTRY vec_cel0_serror
    b       vec_unhandled

/* ---- Group 1: Current EL with SP_ELx (kernel) ---- */
VECTOR_ENTRY vec_celx_sync
    SAVE_CONTEXT
    mov     x0, sp              /* frame pointer */
    mrs     x1, esr_el1         /* exception syndrome */
    bl      arm64_sync_handler
    RESTORE_CONTEXT
    eret

VECTOR_ENTRY vec_celx_irq
    SAVE_CONTEXT
    mov     x0, sp
    bl      arm64_irq_handler
    RESTORE_CONTEXT
    eret

VECTOR_ENTRY vec_celx_fiq
    b       vec_unhandled

VECTOR_ENTRY vec_celx_serror
    b       vec_unhandled

/* ---- Group 2: Lower EL, AArch64 (user-space) ---- */
VECTOR_ENTRY vec_lower64_sync
    SAVE_CONTEXT
    mov     x0, sp
    mrs     x1, esr_el1
    bl      arm64_sync_handler
    RESTORE_CONTEXT
    eret

VECTOR_ENTRY vec_lower64_irq
    SAVE_CONTEXT
    mov     x0, sp
    bl      arm64_irq_handler
    RESTORE_CONTEXT
    eret

VECTOR_ENTRY vec_lower64_fiq
    b       vec_unhandled

VECTOR_ENTRY vec_lower64_serror
    b       vec_unhandled

/* ---- Group 3: Lower EL, AArch32 (not supported) ---- */
VECTOR_ENTRY vec_lower32_sync
    b       vec_unhandled
VECTOR_ENTRY vec_lower32_irq
    b       vec_unhandled
VECTOR_ENTRY vec_lower32_fiq
    b       vec_unhandled
VECTOR_ENTRY vec_lower32_serror
    b       vec_unhandled

/*
 * Unhandled exception — halt
 */
vec_unhandled:
    wfi
    b       vec_unhandled
