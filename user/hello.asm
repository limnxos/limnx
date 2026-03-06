; Limnx — User-space test program (ELF64)
; Prints a message via sys_write, then exits via sys_exit

%include "syscall.inc"

section .text
global _start

_start:
    ; sys_write(msg, msg_len)
    mov rax, SYS_WRITE
    lea rdi, [rel msg]
    mov rsi, msg_len
    syscall

    ; sys_exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall

    ; Should never reach here
    jmp $

section .rodata
msg: db "Hello from user-space!", 10
msg_len equ $ - msg
