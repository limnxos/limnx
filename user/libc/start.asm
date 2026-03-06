; Limnx — C runtime entry point
; _start receives argc in RDI, argv in RSI from kernel.
; Passes them to main(), then SYS_EXIT with the return value.

section .text
bits 64

extern main

global _start
_start:
    ; Save argc/argv in callee-saved registers
    mov r12, rdi        ; r12 = argc
    mov r13, rsi        ; r13 = argv

    ; Align stack to 16 bytes (ABI requirement before call)
    and rsp, ~0xF

    ; Pass argc/argv to main(int argc, char **argv)
    mov rdi, r12        ; 1st arg = argc
    mov rsi, r13        ; 2nd arg = argv
    call main

    ; SYS_EXIT(return value)
    ; rax has main's return value, move to rdi (arg1)
    mov rdi, rax
    mov rax, 2          ; SYS_EXIT = 2
    syscall

    ; Should never reach here
    cli
    hlt
    jmp $
