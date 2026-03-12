/*
 * ARM64 boot initialization
 *
 * Corresponds to x86_64 boot.c.
 * arch_early_init: FPU/NEON, exception vectors, GIC
 * arch_late_init:  (no TSS equivalent on ARM64)
 */

#include "arch/boot.h"
#include "arch/cpu.h"
#include "arch/serial.h"

void arch_early_init(void) {
    /* Enable NEON/FP (equivalent of x86 FPU init) */
    arch_fpu_init();

    serial_puts("[boot] ARM64 early init complete (NEON enabled)\n");
}

void arch_late_init(void) {
    /* ARM64 has no TSS equivalent.
     * Kernel SP is managed via SP_EL1 automatically. */
    serial_puts("[boot] ARM64 late init complete\n");
}
