; Limnx — User-space test program (ELF64)
; Prints a message via write(fd, buf, len), then exits

%include "syscall.inc"

section .text
global _start

_start:
    ; write(fd=1, msg, msg_len)
    mov rax, SYS_WRITE
    mov rdi, 1              ; fd = stdout
    lea rsi, [rel msg]
    mov rdx, msg_len
    syscall

    ; exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
    jmp $

section .rodata
msg: db "Hello from user-space!", 10
msg_len equ $ - msg
