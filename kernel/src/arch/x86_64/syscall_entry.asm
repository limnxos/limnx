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
; Linux preserves all registers across syscall EXCEPT rax (return value),
; rcx (clobbered by SYSCALL hw), r11 (clobbered by SYSCALL hw).
; We must preserve: rdi, rsi, rdx, r8, r9, r10, rbx, rbp, r12-r15, rsp.
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

    ; Save ALL user registers that must be preserved across syscall.
    ; Stack layout (top to bottom):
    ;   user RSP, user RIP (rcx), user RFLAGS (r11),
    ;   rbp, rbx, r12, r13, r14, r15,  (callee-saved)
    ;   rdi, rsi, rdx, r10, r8, r9     (caller-saved, must preserve for Linux compat)

    push qword [gs:8]              ; user RSP
    push rcx                        ; user RIP
    push r11                        ; user RFLAGS

    ; Callee-saved (C ABI)
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Caller-saved user registers (Linux requires these preserved)
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; Set up C call arguments:
    ; syscall_dispatch(num, a1, a2, a3, a4, a5) via System V ABI
    mov r12, rdi                    ; save a1 (user rdi)
    mov r13, rsi                    ; save a2 (user rsi)
    mov r14, rdx                    ; save a3 (user rdx)

    mov rdi, rax                    ; arg0 = syscall number
    mov rsi, r12                    ; arg1 = user rdi
    mov rdx, r13                    ; arg2 = user rsi
    mov rcx, r14                    ; arg3 = user rdx
    mov r8, r10                     ; arg4 = user r10
    mov r9, [rsp+40]               ; arg5 = user r8 (from stack: r8 is at rsp+8*5... wait)
    ; Actually r8 was pushed 3rd from top of caller-saved block.
    ; Stack: r9(top), r8, r10, rdx, rsi, rdi...
    ; So user r8 = [rsp+8] (r8 is second from top)
    ; But we already clobbered r8 with r10 above. Use the stack:
    mov r9, [rsp+8]                 ; arg5 = saved user r8

    ; Enable interrupts during syscall handling
    sti

    call syscall_dispatch
    ; Return value is in RAX — will be returned to user

    ; Disable interrupts for SYSRET sequence
    cli

    ; Restore caller-saved user registers
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi

    ; Restore callee-saved
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Restore user context for SYSRET
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
