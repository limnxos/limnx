#include "arch/smp_hal.h"
#include "arch/percpu.h"
#include "arch/serial.h"

/* Single-core stub for ARM64 */
percpu_t percpu_array[MAX_CPUS] __attribute__((aligned(64)));
uint32_t cpu_count = 1;
uint32_t bsp_cpu_id = 0;

void arch_smp_init(void) {
    percpu_array[0].cpu_id = 0;
    percpu_array[0].started = 1;
    serial_puts("[smp]  ARM64 SMP init (single-core stub)\n");
}

void arch_send_ipi(uint32_t target_cpu_id, uint32_t vector) {
    (void)target_cpu_id; (void)vector;
}

void arch_tlb_shootdown(void) {
    /* Single-core: no-op */
}

void arch_syscall_init(void) {
    serial_puts("[svc]  ARM64 syscall init (stub)\n");
}
