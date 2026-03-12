; Limnx — User-space writable VFS test (ELF64)
; Creates /userfile.txt, writes data, reopens, reads back, prints.

%include "syscall.inc"

section .text
global _start

_start:
    ; Print start message
    mov rax, SYS_WRITE
    lea rdi, [rel msg_start]
    mov rsi, msg_start_len
    syscall

    ; sys_unlink("/userfile.txt") — remove if left over from previous boot
    mov rax, SYS_UNLINK
    lea rdi, [rel filepath]
    syscall
    ; ignore error (file may not exist)

    ; sys_create("/userfile.txt") -> fd in r12
    mov rax, SYS_CREATE
    lea rdi, [rel filepath]
    syscall
    test rax, rax
    js .fail
    mov r12, rax

    ; sys_fwrite(fd, data, len)
    mov rax, SYS_FWRITE
    mov rdi, r12
    lea rsi, [rel write_data]
    mov rdx, write_data_len
    syscall
    test rax, rax
    js .fail

    ; sys_close(fd)
    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall

    ; Reopen: sys_open("/userfile.txt") -> fd in r12
    mov rax, SYS_OPEN
    lea rdi, [rel filepath]
    xor rsi, rsi
    syscall
    test rax, rax
    js .fail
    mov r12, rax

    ; sys_read(fd, buf, 256)
    mov rax, SYS_READ
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 256
    syscall
    test rax, rax
    jle .fail
    mov r13, rax            ; save bytes read

    ; sys_write(buf, bytes_read) — print to console
    mov rax, SYS_WRITE
    lea rdi, [rel buf]
    mov rsi, r13
    syscall

    ; Print newline
    mov rax, SYS_WRITE
    lea rdi, [rel newline]
    mov rsi, 1
    syscall

    ; Print success message
    mov rax, SYS_WRITE
    lea rdi, [rel msg_ok]
    mov rsi, msg_ok_len
    syscall

    ; sys_close(fd)
    mov rax, SYS_CLOSE
    mov rdi, r12
    syscall

    ; sys_exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
    jmp $

.fail:
    mov rax, SYS_WRITE
    lea rdi, [rel msg_fail]
    mov rsi, msg_fail_len
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
