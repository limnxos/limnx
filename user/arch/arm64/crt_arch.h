#ifndef LIMNX_USER_ARM64_CRT_ARCH_H
#define LIMNX_USER_ARM64_CRT_ARCH_H

/* ARM64 _start: kernel passes argc in x0, argv in x1 */
__asm__(
    ".section .text\n"
    ".global _start\n"
    "_start:\n"
    "    mov x29, #0\n"             /* clear frame pointer */
    "    mov x30, #0\n"             /* clear link register */
    "    and sp, sp, #-16\n"        /* 16-byte align stack */
    "    bl __libc_start\n"         /* __libc_start(argc=x0, argv=x1) */
    "    mov x8, #2\n"             /* SYS_EXIT = 2 */
    "    svc #0\n"
    "    wfi\n"
    "    b .\n"
);

#endif
