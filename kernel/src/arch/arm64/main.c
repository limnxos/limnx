/*
 * ARM64 kernel entry point (minimal boot for QEMU virt machine).
 * Initializes PL011 UART and prints boot banner.
 * Full subsystem init (PMM, VMM, scheduler, etc.) is future work.
 */

#include "serial.h"
#include "arch/cpu.h"

void kmain(void) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — ARM64 (aarch64)\n");
    serial_puts("  Target: QEMU virt (Cortex-A57)\n");
    serial_puts("========================================\n\n");

    serial_puts("[init] PL011 UART initialized\n");

    /* Enable FPU/NEON */
    arch_fpu_init();
    serial_puts("[fpu]  FPU/NEON enabled (CPACR_EL1.FPEN=11)\n");

    serial_puts("\n[init] ARM64 boot complete — halting\n");
    serial_puts("[init] (full subsystem init is future work)\n");

    /* Halt */
    arch_irq_disable();
    for (;;)
        arch_halt();
}
