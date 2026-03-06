; Limnx — User-space UDP echo server (ELF64)
; Binds UDP port 1234, receives a packet, prints it, echoes it back, then exits.

%include "syscall.inc"

section .text
global _start

_start:
    ; Print banner
    mov rax, SYS_WRITE
    lea rdi, [rel banner]
    mov rsi, banner_len
    syscall

    ; sockfd = socket()
    mov rax, SYS_SOCKET
    syscall
    test rax, rax
    js .fail
    mov r12, rax            ; r12 = sockfd

    ; bind(sockfd, 1234)
    mov rax, SYS_BIND
    mov rdi, r12
    mov rsi, 1234
    syscall
    test rax, rax
    js .fail

    ; Print "listening" message
    mov rax, SYS_WRITE
    lea rdi, [rel listening]
    mov rsi, listening_len
    syscall

    ; n = recvfrom(sockfd, buf, 1024, &src_ip, &src_port)
    mov rax, SYS_RECVFROM
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, 1024
    lea r10, [rel src_ip]
    lea r8,  [rel src_port]
    syscall
    test rax, rax
    jle .fail
    mov r13, rax            ; r13 = bytes received

    ; Print "received:" prefix
    mov rax, SYS_WRITE
    lea rdi, [rel recv_msg]
    mov rsi, recv_msg_len
    syscall

    ; Print received data
    mov rax, SYS_WRITE
    lea rdi, [rel buf]
    mov rsi, r13
    syscall

    ; Print newline
    mov rax, SYS_WRITE
    lea rdi, [rel newline]
    mov rsi, 1
    syscall

    ; Echo back: sendto(sockfd, buf, n, src_ip, src_port)
    mov rax, SYS_SENDTO
    mov rdi, r12
    lea rsi, [rel buf]
    mov rdx, r13
    mov r10d, [rel src_ip]
    movzx r8, word [rel src_port]
    syscall

    ; Exit(0)
    mov rax, SYS_EXIT
    xor rdi, rdi
    syscall
    jmp $

.fail:
    ; Print error
    mov rax, SYS_WRITE
    lea rdi, [rel err_msg]
    mov rsi, err_msg_len
    syscall

    ; Exit(1)
    mov rax, SYS_EXIT
    mov rdi, 1
    syscall
    jmp $

section .rodata
banner:      db "udpecho: starting", 10
banner_len   equ $ - banner
listening:   db "udpecho: listening on port 1234", 10
listening_len equ $ - listening
recv_msg:    db "udpecho: received: "
recv_msg_len equ $ - recv_msg
err_msg:     db "udpecho: error!", 10
err_msg_len  equ $ - err_msg
newline:     db 10

section .bss
buf:      resb 1024
src_ip:   resd 1
src_port: resw 1
