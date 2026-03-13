#ifndef LIMNX_ARCH_CPU_H
#define LIMNX_ARCH_CPU_H

/*
 * Architecture-independent CPU interface.
 *
 * Each architecture provides static inline implementations of:
 *   arch_halt()              — halt CPU until next interrupt
 *   arch_pause()             — yield to hypervisor / save power in spin loop
 *   arch_irq_enable()        — enable interrupts
 *   arch_irq_disable()       — disable interrupts
 *   arch_irq_save()          — save interrupt state + disable (returns flags)
 *   arch_irq_restore(flags)  — restore interrupt state
 *   arch_fpu_init()          — enable FPU/SIMD on this CPU
 *   arch_fpu_save(buf)       — save FPU state to 512-byte aligned buffer
 *   arch_fpu_restore(buf)    — restore FPU state from buffer
 *   arch_set_tls_base(base)  — set thread-local storage base register
 *   arch_get_tls_base()      — get thread-local storage base register
 *   arch_memory_barrier()    — compiler + memory barrier
 *   arch_breakpoint()        — trigger debug breakpoint exception
 *   arch_prepare_usermode_return() — prepare CPU state for return to usermode
 *   arch_enter_usermode(entry, rsp, rdi, rsi) — enter ring 3 (noreturn)
 *   arch_enter_forked_child(fork_ctx) — enter forked child process (noreturn)
 */

#if defined(__x86_64__)
#include "arch/x86_64/cpu.h"
#elif defined(__aarch64__)
#include "arch/arm64/cpu.h"
#else
#error "Unsupported architecture"
#endif

#endif /* LIMNX_ARCH_CPU_H */
