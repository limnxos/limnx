; Limnx — SYSCALL entry/exit stub (x86_64 NASM)
;
; On SYSCALL entry, CPU has:
;   RCX = user RIP (return address)
;   R11 = user RFLAGS
;   CS/SS = kernel segments (from STAR MSR)
;   Interrupts disabled (SFMASK cleared IF)
;   RSP = STILL the user stack (not swapped!)
;
; Syscall convention:
;   RAX = syscall number
;   RDI = arg1, RSI = arg2, RDX = arg3, R10 = arg4, R8 = arg5, R9 = arg6
;   Return value in RAX
;
; Per-CPU data is accessed via GS after SWAPGS.
; Layout (percpu_t):
;   [gs:0]  = kernel_rsp
;   [gs:8]  = user_rsp_save
;   [gs:16] = self pointer
;   [gs:48] = signal_deliver_pending
;   [gs:56] = signal_deliver_rdi

section .text
bits 64

extern syscall_dispatch

global syscall_entry
syscall_entry:
    ; SWAPGS: switch from user GS to kernel GS (per-CPU data)
    swapgs

    ; Save user RSP into per-CPU slot, load kernel RSP
    mov [gs:8], rsp
    mov rsp, [gs:0]

    ; Build a stack frame for the C handler
    ; Save user context we need to restore on SYSRET
    push qword [gs:8]              ; user RSP
    push rcx                        ; user RIP
    push r11                        ; user RFLAGS

    ; Save callee-saved registers (C ABI requires we preserve these)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save syscall args that overlap with C call-clobbered regs
    ; We need to re-arrange: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5
    ; syscall_dispatch(num, a1, a2, a3, a4, a5) via System V ABI:
    ;   rdi=num, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5
    mov r12, rdi                    ; save a1
    mov r13, rsi                    ; save a2
    mov r14, rdx                    ; save a3
    mov r15, r8                     ; save a5 (user r8, before r8 is overwritten)

    mov rdi, rax                    ; arg0 = syscall number
    mov rsi, r12                    ; arg1 = user rdi
    mov rdx, r13                    ; arg2 = user rsi
    mov rcx, r14                    ; arg3 = user rdx
    mov r8, r10                     ; arg4 = user r10
    mov r9, r15                     ; arg5 = user r8

    ; Enable interrupts during syscall handling
    sti

    call syscall_dispatch
    ; Return value is in RAX — will be returned to user

    ; Disable interrupts for SYSRET sequence
    cli

    ; Restore callee-saved
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Restore user context
    pop r11                         ; user RFLAGS
    pop rcx                         ; user RIP
    pop rsp                         ; user RSP

    ; Check if we need to deliver a signal
    mov r10b, [gs:48]
    test r10b, r10b
    jz .no_signal
    mov byte [gs:48], 0
    mov rdi, [gs:56]              ; signal number for handler arg
    mov rcx, [gs:64]              ; override RIP → signal handler
    mov rsp, [gs:72]              ; override RSP → signal frame
.no_signal:

    ; SWAPGS back to user GS before returning
    swapgs

    ; Return to user-space
    o64 sysret
