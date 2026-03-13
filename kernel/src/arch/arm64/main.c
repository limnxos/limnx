/*
 * ARM64 kernel entry point — full subsystem boot
 * Mirrors x86_64 main.c init sequence using shared kernel code.
 */

#include "arch/serial.h"
#include "arch/cpu.h"
#include "arch/boot.h"
#include "arch/interrupt.h"
#include "arch/smp_hal.h"
#include "arch/timer.h"
#include "arch/syscall_arch.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "fs/vfs.h"
#include "pty/pty.h"
#include "sync/futex.h"

void kmain(void) {
    serial_init();

    serial_puts("\n========================================\n");
    serial_puts("  Limnx Kernel — ARM64 (aarch64)\n");
    serial_puts("  Target: QEMU virt (Cortex-A57)\n");
    serial_puts("========================================\n\n");

    /* === Stage 1: Arch early init === */
    arch_early_init();
    arch_interrupt_init();

    /* === Stage 2: Physical memory === */
    serial_puts("\n--- PMM init ---\n");
    pmm_init();

    /* PMM smoke test */
    {
        uint64_t p = pmm_alloc_page();
        if (p) {
            serial_puts("[test] PMM smoke test PASSED\n");
            pmm_free_page(p);
        } else {
            serial_puts("[test] PMM smoke test FAILED\n");
        }
    }

    /* === Stage 3: VMM + heap === */
    serial_puts("\n--- VMM + kheap init ---\n");
    vmm_init();
    kheap_init();

    /* Heap smoke test */
    {
        void *p = kmalloc(128);
        if (p) {
            serial_puts("[test] Heap smoke test PASSED\n");
            kfree(p);
        } else {
            serial_puts("[test] Heap smoke test FAILED\n");
        }
    }

    /* === Stage 4: Scheduler === */
    serial_puts("\n--- Scheduler init ---\n");
    arch_late_init();
    sched_init();
    arch_timer_enable_sched();

    /* Scheduler smoke test */
    {
        extern void sched_smoke_test(void);
        sched_smoke_test();
    }

    /* === Stage 5: Syscalls === */
    serial_puts("\n--- Syscall init ---\n");
    arch_syscall_init();
    syscall_init();

    /* === Stage 6: VFS === */
    serial_puts("\n--- VFS init ---\n");
    vfs_init();
    /* No initrd on ARM64 (no Limine module loading) */

    /* === Stage 7: IPC + PTY === */
    serial_puts("\n--- IPC init ---\n");
    pty_init();
    futex_init();

    /* === Stage 8: SMP === */
    arch_smp_init();

    serial_puts("\n========================================\n");
    serial_puts("  ARM64 boot complete — all subsystems\n");
    serial_puts("========================================\n");

    /* Halt — no user-space shell on ARM64 yet */
    arch_irq_disable();
    for (;;)
        arch_halt();
}
