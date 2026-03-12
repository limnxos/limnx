#ifndef LIMNX_ARCH_CONTEXT_H
#define LIMNX_ARCH_CONTEXT_H

/*
 * Architecture-independent context switch interface.
 * Each architecture provides these in assembly.
 * x86_64: switch.asm (fxsave/fxrstor + callee-saved regs)
 * ARM64:  switch.S (NEON save/restore + callee-saved regs)
 */

#include <stdint.h>

extern void context_switch(void *old_ctx_ptr, void *new_ctx_ptr,
                           void *old_fpu_state, void *new_fpu_state);
extern void thread_trampoline(void);

#endif
