/*
 * ARM64 user-mode entry routines
 *
 * arch_enter_usermode_asm(entry, sp, argc, argv)
 *   — Enter user mode via ERET: sets ELR_EL1, SP_EL0, SPSR_EL1
 *
 * arch_enter_forked_child_asm(fork_ctx_ptr)
 *   — Enter user mode as forked child: restore fork_context_t, x0=0, ERET
 */

.section .text

/*
 * void arch_enter_usermode_asm(uint64_t entry, uint64_t sp,
 *                               uint64_t argc, uint64_t argv)
 *   x0 = entry point (user PC)
 *   x1 = user stack pointer
 *   x2 = argc
 *   x3 = argv pointer
 */
.global arch_enter_usermode_asm
arch_enter_usermode_asm:
    /* Disable interrupts — ELR/SPSR must not be written with IRQs enabled.
     * A timer IRQ between msr elr_el1 and eret would clobber these registers.
     * ERET restores PSTATE from SPSR (which has interrupts unmasked for EL0). */
    msr     daifset, #0xf

    /* Flush I-cache: D/I caches not coherent, must sync after loading ELF */
    ic      iallu
    dsb     ish
    isb

    msr     elr_el1, x0         /* Return address = entry */
    msr     sp_el0, x1          /* User stack pointer */
    mov     x0, x2              /* x0 = argc */
    mov     x1, x3              /* x1 = argv */

    /* SPSR_EL1 = 0: EL0t (user mode), all exceptions unmasked */
    msr     spsr_el1, xzr

    /* Clear all other registers for clean entry */
    mov     x2, #0
    mov     x3, #0
    mov     x4, #0
    mov     x5, #0
    mov     x6, #0
    mov     x7, #0
    mov     x8, #0
    mov     x9, #0
    mov     x10, #0
    mov     x11, #0
    mov     x12, #0
    mov     x13, #0
    mov     x14, #0
    mov     x15, #0
    mov     x16, #0
    mov     x17, #0
    mov     x18, #0
    mov     x19, #0
    mov     x20, #0
    mov     x21, #0
    mov     x22, #0
    mov     x23, #0
    mov     x24, #0
    mov     x25, #0
    mov     x26, #0
    mov     x27, #0
    mov     x28, #0
    mov     x29, #0
    mov     x30, #0

    eret

/*
 * void arch_enter_forked_child_asm(const fork_context_t *ctx)
 *   x0 = pointer to fork_context_t
 *
 * fork_context_t layout (ARM64) — full register set like Linux's pt_regs:
 *   offset   0: x[0]..x[30]  (31 × 8 = 248 bytes)
 *   offset 248: elr  (saved PC)
 *   offset 256: sp   (saved user SP)
 *   offset 264: spsr (saved PSTATE)
 */
.global arch_enter_forked_child_asm
arch_enter_forked_child_asm:
    /* Disable interrupts — ELR/SPSR writes must be atomic with ERET */
    msr     daifset, #0xf

    /* Load system registers */
    ldr     x1, [x0, #248]     /* elr */
    ldr     x2, [x0, #256]     /* sp */
    ldr     x3, [x0, #264]     /* spsr */
    msr     elr_el1, x1
    msr     sp_el0, x2
    msr     spsr_el1, x3

    /* Restore ALL general-purpose registers x1-x30 from fork context.
     * x0 is set to 0 (fork return value) AFTER restoring others. */
    ldp     x1,  x2,  [x0, #8]
    ldp     x3,  x4,  [x0, #24]
    ldp     x5,  x6,  [x0, #40]
    ldp     x7,  x8,  [x0, #56]
    ldp     x9,  x10, [x0, #72]
    ldp     x11, x12, [x0, #88]
    ldp     x13, x14, [x0, #104]
    ldp     x15, x16, [x0, #120]
    ldp     x17, x18, [x0, #136]
    ldp     x19, x20, [x0, #152]
    ldp     x21, x22, [x0, #168]
    ldp     x23, x24, [x0, #184]
    ldp     x25, x26, [x0, #200]
    ldp     x27, x28, [x0, #216]
    ldp     x29, x30, [x0, #232]

    /* Child returns 0 from fork */
    mov     x0, #0

    eret
