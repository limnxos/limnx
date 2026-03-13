#ifndef LIMNX_ARCH_SYSCALL_H
#define LIMNX_ARCH_SYSCALL_H

#include <stdint.h>

/*
 * Architecture-independent syscall entry setup.
 * x86_64: STAR/LSTAR/SFMASK MSRs for SYSCALL instruction
 * ARM64:  EL1 exception vector table for SVC instruction
 */

void arch_syscall_init(void);
void arch_set_kernel_stack(uint64_t stack_top);

/*
 * Kernel stack frame offsets for signal delivery.
 * These match the layout pushed by the arch-specific syscall entry stub.
 * kstack_top[offset] gives the saved user register value.
 *
 * x86_64 syscall_entry.asm pushes: user_RSP, user_RIP, user_RFLAGS
 *   then callee-saved regs. So from kstack_top (top of allocation):
 *   [-1] = user RSP, [-2] = user RIP, [-3] = user RFLAGS
 *
 * ARM64 vectors.S pushes: x0-x30, ELR_EL1, SPSR_EL1, SP_EL0
 *   Offsets differ — defined per arch.
 */
#if defined(__x86_64__)
#define KSTACK_USER_RIP_OFF     (-2)
#define KSTACK_USER_RSP_OFF     (-1)
#define KSTACK_USER_FLAGS_OFF   (-3)
#elif defined(__aarch64__)
#define KSTACK_USER_RIP_OFF     (-3)  /* ELR_EL1 */
#define KSTACK_USER_RSP_OFF     (-1)  /* SP_EL0 */
#define KSTACK_USER_FLAGS_OFF   (-2)  /* SPSR_EL1 */
#endif

#endif
