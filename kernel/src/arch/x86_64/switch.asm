; Limnx — Context switch and thread trampoline (x86_64 NASM)

section .text
bits 64

; ============================================================
; context_switch(old_ctx_ptr, new_ctx_ptr, old_fpu_state, new_fpu_state)
;   rdi = &old_thread->context  (pointer to cpu_context_t*)
;   rsi = &new_thread->context  (pointer to cpu_context_t*)
;   rdx = old_thread->fpu_state (pointer to 512-byte FXSAVE area)
;   rcx = new_thread->fpu_state (pointer to 512-byte FXSAVE area)
;
; Saves FPU/SSE state, callee-saved regs on old stack, swaps RSP,
; restores callee-saved regs from new stack, restores FPU/SSE state,
; ret into new thread.
; ============================================================
global context_switch
context_switch:
    ; Save FPU/SSE state into old thread's fpu_state buffer
    fxsave [rdx]

    ; Save callee-saved registers on OLD thread's stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save old stack pointer into old_thread->context
    mov [rdi], rsp

    ; Load new stack pointer from new_thread->context
    mov rsp, [rsi]

    ; Restore callee-saved registers from NEW thread's stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Restore FPU/SSE state from new thread's fpu_state buffer
    fxrstor [rcx]

    ; 'ret' pops RIP — jumps to new thread's saved execution point
    ret

; ============================================================
; thread_trampoline
;   Called when a new thread runs for the first time.
;   rbx = entry function pointer (set up by thread_create)
;
;   Moves rbx → rdi (first argument per System V ABI),
;   then calls thread_entry_wrapper(entry).
; ============================================================
extern thread_entry_wrapper
extern sched_unlock_after_switch
global thread_trampoline
thread_trampoline:
    ; New thread: release the scheduler lock held across context_switch
    call sched_unlock_after_switch
    mov rdi, rbx
    call thread_entry_wrapper
    ; Should never return, but just in case:
    cli
    hlt
    jmp thread_trampoline
