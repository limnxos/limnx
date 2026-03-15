; Limnx — User-space writable VFS test (ELF64)
; Creates /userfile.txt, writes data, reopens, reads back, prints.

%include "syscall.inc"

section .text
global _start

_start:
    ; Print start message — write(1, msg, len)
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel msg_start]
    mov rdx, msg_start_len
    syscall

    ; unlink("/userfile.txt") — remove if left over
    mov rax, SYS_UNLINK
    lea rdi, [rel filepath]
    syscall

    ; creat("/userfile.txt") -> fd in r12
    mov rax, SYS_CREAT
    lea rdi, [rel filepath]
    syscall
    test rax, rax
    js .fail
    mov r12, rax

    ; write(fd, data, len)
    mov rax, SYS_WRITE
    mov rdi, r12
    lea rsi, [rel write_data]
    mov rdx, write_data_len
    syscall
    test rax, rax
    js .fail

    ; close(fd)
    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall

    ; open("/userfile.txt", 0) -> fd in r12
    mov rax, SYS_OPEN
    lea rdi, [rel filepath]
    xor rsi, rsi
    syscall
    test rax, rax
    js .fail
    mov r12, rax

    ; read(fd, buf, 256)
    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 256
    syscall
    test rax, rax
    jle .fail
    mov r13, rax

    ; write(1, buf, bytes_read)
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel buf]
    mov rdx, r13
    syscall

    ; write(1, newline, 1)
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel newline]
    mov rdx, 1
    syscall

    ; Print success
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel msg_ok]
    mov rdx, msg_ok_len
    syscall

    ; close(fd)
    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall

    ; exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
    jmp $

.fail:
    mov rax, SYS_WRITE
    mov rdi, 1
    lea rsi, [rel msg_fail]
    mov rdx, msg_fail_len
    syscall

    mov rax, SYS_EXIT
    mov rdi, 1
    syscall
    jmp $

section .rodata
filepath:    db "/userfile.txt", 0
write_data:  db "Hello from user-space writable VFS!"
write_data_len equ $ - write_data
msg_start:   db "[writetest] Starting write test...", 10
msg_start_len equ $ - msg_start
msg_ok:      db "[writetest] Write test PASSED", 10
msg_ok_len   equ $ - msg_ok
msg_fail:    db "[writetest] Write test FAILED", 10
msg_fail_len equ $ - msg_fail
newline:     db 10

section .bss
buf: resb 256
