/*
 * ARM64 SMP support — single-core stub with GIC SGI for IPI
 *
 * Corresponds to x86_64 smp.c.
 * Uses PSCI for CPU bringup (future), GIC SGI for IPIs.
 */

#include "arch/smp_hal.h"
#include "arch/percpu.h"
#include "arch/arm64/gic.h"
#include "arch/serial.h"

/* Global per-CPU data (same as x86_64) */
percpu_t percpu_array[MAX_CPUS] __attribute__((aligned(64)));
uint32_t cpu_count = 1;
uint32_t bsp_cpu_id = 0;

/* SGI IDs for IPI */
#define SGI_TLB_SHOOTDOWN  1
#define SGI_RESCHEDULE     2

void arch_smp_init(void) {
    percpu_array[0].cpu_id = 0;
    percpu_array[0].started = 1;

    /* Set TPIDR_EL1 to point to BSP percpu data */
    uint64_t base = (uint64_t)&percpu_array[0];
    __asm__ volatile ("msr tpidr_el1, %0" : : "r"(base));

    serial_puts("[smp]  ARM64 SMP init (single-core, PSCI bringup stub)\n");

    /* TODO: enumerate CPUs via device tree, PSCI CPU_ON to bring up APs */
}

void arch_send_ipi(uint32_t target_cpu_id, uint32_t vector) {
    /* Map vector to SGI ID */
    uint32_t sgi_id = (vector == SGI_TLB_SHOOTDOWN) ? SGI_TLB_SHOOTDOWN : vector;
    gic_send_sgi(target_cpu_id, sgi_id);
}

void arch_tlb_shootdown(void) {
    /*
     * ARM64: TLBI broadcasts are handled by hardware on most implementations.
     * TLBI VMALLE1 invalidates all TLB entries for EL1 on all PEs in the
     * same inner-shareable domain.
     *
     * No IPI needed — use broadcast TLBI.
     */
    __asm__ volatile (
        "dsb ishst\n"       /* Ensure PTE writes are visible */
        "tlbi vmalle1is\n"  /* Invalidate all EL1 TLB, inner-shareable */
        "dsb ish\n"         /* Wait for TLBI to complete */
        "isb\n"             /* Synchronize context */
    );
}

void arch_syscall_init(void) {
    /* ARM64: SVC handler is installed via VBAR_EL1 in arch_interrupt_init.
     * No MSR setup needed (unlike x86_64 STAR/LSTAR/SFMASK). */
    serial_puts("[svc]  ARM64 syscall init (via VBAR_EL1 vector table)\n");
}

void arch_set_kernel_stack(uint64_t stack_top) {
    (void)stack_top;
    /* ARM64: SP_EL1 is set automatically by exception entry */
}
