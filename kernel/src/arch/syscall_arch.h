#ifndef LIMNX_ARCH_SYSCALL_H
#define LIMNX_ARCH_SYSCALL_H

/*
 * Architecture-independent syscall entry setup.
 * x86_64: STAR/LSTAR/SFMASK MSRs for SYSCALL instruction
 * ARM64:  EL1 exception vector table for SVC instruction
 */

void arch_syscall_init(void);

#endif
