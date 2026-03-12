/*
 * ARM64 context switch and thread trampoline
 *
 * Corresponds to x86_64 switch.asm.
 *
 * context_switch(old_ctx_ptr, new_ctx_ptr, old_fpu_state, new_fpu_state)
 *   x0 = &old_thread->context  (pointer to cpu_context_t*)
 *   x1 = &new_thread->context  (pointer to cpu_context_t*)
 *   x2 = old_thread->fpu_state (pointer to NEON save area)
 *   x3 = new_thread->fpu_state (pointer to NEON save area)
 *
 * ARM64 AAPCS64 callee-saved: x19-x28, x29 (FP), x30 (LR), SP
 */

.section .text
.global context_switch
context_switch:
    /* Save NEON/FP state into old thread's fpu_state buffer */
    stp     q0,  q1,  [x2, #0]
    stp     q2,  q3,  [x2, #32]
    stp     q4,  q5,  [x2, #64]
    stp     q6,  q7,  [x2, #96]
    stp     q8,  q9,  [x2, #128]
    stp     q10, q11, [x2, #160]
    stp     q12, q13, [x2, #192]
    stp     q14, q15, [x2, #224]
    stp     q16, q17, [x2, #256]
    stp     q18, q19, [x2, #288]
    stp     q20, q21, [x2, #320]
    stp     q22, q23, [x2, #352]
    stp     q24, q25, [x2, #384]
    stp     q26, q27, [x2, #416]
    stp     q28, q29, [x2, #448]
    stp     q30, q31, [x2, #480]

    /* Save callee-saved registers on old thread's stack */
    stp     x19, x20, [sp, #-16]!
    stp     x21, x22, [sp, #-16]!
    stp     x23, x24, [sp, #-16]!
    stp     x25, x26, [sp, #-16]!
    stp     x27, x28, [sp, #-16]!
    stp     x29, x30, [sp, #-16]!

    /* Save old SP into old_thread->context */
    mov     x9, sp
    str     x9, [x0]

    /* Load new SP from new_thread->context */
    ldr     x9, [x1]
    mov     sp, x9

    /* Restore callee-saved registers from new thread's stack */
    ldp     x29, x30, [sp], #16
    ldp     x27, x28, [sp], #16
    ldp     x25, x26, [sp], #16
    ldp     x23, x24, [sp], #16
    ldp     x21, x22, [sp], #16
    ldp     x19, x20, [sp], #16

    /* Restore NEON/FP state from new thread's fpu_state buffer */
    ldp     q0,  q1,  [x3, #0]
    ldp     q2,  q3,  [x3, #32]
    ldp     q4,  q5,  [x3, #64]
    ldp     q6,  q7,  [x3, #96]
    ldp     q8,  q9,  [x3, #128]
    ldp     q10, q11, [x3, #160]
    ldp     q12, q13, [x3, #192]
    ldp     q14, q15, [x3, #224]
    ldp     q16, q17, [x3, #256]
    ldp     q18, q19, [x3, #288]
    ldp     q20, q21, [x3, #320]
    ldp     q22, q23, [x3, #352]
    ldp     q24, q25, [x3, #384]
    ldp     q26, q27, [x3, #416]
    ldp     q28, q29, [x3, #448]
    ldp     q30, q31, [x3, #480]

    /* Return into new thread (via restored LR/x30) */
    ret

/*
 * thread_trampoline
 *   Called when a new thread runs for the first time.
 *   x19 = entry function pointer (set up by thread_create, preserved across context_switch)
 *
 *   Unlocks the scheduler, then calls thread_entry_wrapper(entry).
 */
.global thread_trampoline
thread_trampoline:
    bl      sched_unlock_after_switch
    mov     x0, x19
    bl      thread_entry_wrapper
    /* Should never return */
    wfi
    b       thread_trampoline
