#include "arch/timer.h"
#include "arch/serial.h"

static volatile uint64_t arm64_ticks = 0;

uint64_t arch_timer_get_ticks(void) {
    return arm64_ticks;
}

void arch_timer_enable_sched(void) {
    serial_puts("[timer] ARM64 timer enable (stub)\n");
}

void arch_timer_disable_sched(void) {
    /* stub */
}
