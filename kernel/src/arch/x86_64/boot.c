#define pr_fmt(fmt) "[boot] " fmt
#include "klog.h"

#include "arch/boot.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/tss.h"
#include "arch/cpu.h"

/*
 * x86_64 early boot initialization.
 * Called before memory subsystem (PMM/VMM/kheap).
 */
void arch_early_init(void) {
    gdt_init();
    idt_init();
    arch_fpu_init();
}

/*
 * x86_64 late boot initialization.
 * Called after scheduler, VFS, and subsystems are ready.
 */
void arch_late_init(void) {
    tss_init();
}
