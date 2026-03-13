#ifndef LIMNX_USER_X86_64_CRT_ARCH_H
#define LIMNX_USER_X86_64_CRT_ARCH_H

/* x86_64 _start: kernel passes argc in rdi, argv in rsi */
__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    xor %rbp, %rbp\n"          /* clear frame pointer */
    "    and $-16, %rsp\n"          /* 16-byte align stack (ABI) */
    "    call __libc_start\n"       /* __libc_start(argc=rdi, argv=rsi) */
    "    mov %rax, %rdi\n"          /* exit code = return value */
    "    mov $2, %rax\n"            /* SYS_EXIT = 2 */
    "    syscall\n"
    "    cli\n"
    "    hlt\n"
);

#endif
