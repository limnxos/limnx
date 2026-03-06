; Limnx — User-space VFS test program (ELF64)
; Opens /hello.txt from initrd, reads it, prints contents, exits.

%include "syscall.inc"

section .text
global _start

_start:
    ; sys_open("/hello.txt", 0)
    mov rax, SYS_OPEN
    lea rdi, [rel path]
    xor rsi, rsi
    syscall

    ; Save fd
    test rax, rax
    js .fail
    mov r12, rax

    ; sys_read(fd, buf, 256)
    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 256
    syscall

    ; rax = bytes read
    test rax, rax
    jle .fail

    ; sys_write(buf, bytes_read)
    mov rsi, rax
    mov rax, SYS_WRITE
    lea rdi, [rel buf]
    syscall

    ; sys_exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
    jmp $

.fail:
    ; sys_exit(1) on error
    mov rax, SYS_EXIT
    mov rdi, 1
    syscall
    jmp $

section .rodata
path: db "/hello.txt", 0

section .bss
buf: resb 256
