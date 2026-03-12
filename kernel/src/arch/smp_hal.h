#ifndef LIMNX_ARCH_SMP_HAL_H
#define LIMNX_ARCH_SMP_HAL_H

/*
 * Architecture-independent SMP interface.
 * x86_64: Limine SMP + LAPIC IPI
 * ARM64:  PSCI / spin-table (stub for now)
 */

#include <stdint.h>

void arch_smp_init(void);
void arch_send_ipi(uint32_t target_cpu_id, uint32_t vector);
void arch_tlb_shootdown(void);

#endif
