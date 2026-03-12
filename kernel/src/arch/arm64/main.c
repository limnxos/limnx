/*
 * ARM64 kernel entry point (minimal boot for QEMU virt machine).
 * Initializes PL011 UART and prints boot banner.
 * Full subsystem init (PMM, VMM, scheduler, etc.) is future work.
 */

#include "arch/serial.h"
#include "arch/cpu.h"
#include "arch/boot.h"
#include "arch/interrupt.h"
#include "arch/smp_hal.h"
#include "arch/timer.h"
#include "arch/syscall_arch.h"

/*
 * Weak stubs for kernel symbols referenced by arch asm/C code.
 * These are provided by the full kernel build; the ARM64-only build
 * uses these stubs to link successfully.
 */
void __attribute__((weak)) sched_unlock_after_switch(void) {}
void __attribute__((weak)) thread_entry_wrapper(void *entry) { (void)entry; }

void kmain(void) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — ARM64 (aarch64)\n");
    serial_puts("  Target: QEMU virt (Cortex-A57)\n");
    serial_puts("========================================\n\n");

    /* HAL init sequence (mirrors x86_64 boot) */
    arch_early_init();
    arch_interrupt_init();
    arch_smp_init();
    arch_syscall_init();
    arch_timer_enable_sched();

    serial_puts("\n[init] ARM64 boot complete — halting\n");
    serial_puts("[init] (PMM, VMM, scheduler, VFS are future work)\n");

    /* Halt */
    arch_irq_disable();
    for (;;)
        arch_halt();
}
