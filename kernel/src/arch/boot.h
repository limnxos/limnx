#ifndef LIMNX_ARCH_BOOT_H
#define LIMNX_ARCH_BOOT_H

/*
 * Architecture-specific boot initialization.
 *
 * arch_early_init() — called before memory subsystem.
 *   x86_64: GDT, IDT, FPU/SSE
 *   ARM64:  VBAR_EL1, FPU/NEON
 *
 * arch_late_init() — called after scheduler/VFS are ready.
 *   x86_64: TSS, SMP (AP bootstrap, LAPIC), SYSCALL MSRs
 *   ARM64:  (stub)
 */

void arch_early_init(void);
void arch_late_init(void);

#endif
