#include "arch/boot.h"
#include "arch/cpu.h"
#include "arch/serial.h"

void arch_early_init(void) {
    arch_fpu_init();
    serial_puts("[boot] ARM64 early init complete (FPU/NEON enabled)\n");
}

void arch_late_init(void) {
    serial_puts("[boot] ARM64 late init (stub)\n");
}
