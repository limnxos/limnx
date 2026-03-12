/*
 * ARM64 generic timer driver
 *
 * Corresponds to x86_64 PIT/LAPIC timer.
 * Uses CNTPCT_EL0 (physical counter) and CNTP_*_EL0 (physical timer).
 *
 * The ARM64 generic timer runs at a frequency readable from CNTFRQ_EL0.
 * QEMU virt default: 62.5 MHz (62500000 Hz).
 */

#include "arch/timer.h"
#include "arch/serial.h"
#include <stdint.h>

static uint64_t timer_freq;
static volatile uint64_t timer_ticks;

static inline uint64_t read_cntfrq(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline uint64_t read_cntpct(void) {
    uint64_t val;
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

static inline void write_cntp_tval(uint64_t val) {
    __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(val));
}

static inline void write_cntp_ctl(uint64_t val) {
    __asm__ volatile ("msr cntp_ctl_el0, %0" : : "r"(val));
}

uint64_t arch_timer_get_ticks(void) {
    if (timer_freq == 0)
        return timer_ticks;
    /* Convert counter to millisecond ticks */
    return read_cntpct() / (timer_freq / 1000);
}

void arch_timer_enable_sched(void) {
    timer_freq = read_cntfrq();

    /* Set timer to fire every 10ms */
    uint64_t interval = timer_freq / 100;  /* 10ms */
    write_cntp_tval(interval);

    /* Enable timer, unmask interrupt */
    write_cntp_ctl(1);  /* ENABLE=1, IMASK=0 */

    serial_printf("[timer] ARM64 generic timer enabled (freq=%lu Hz, 10ms interval)\n",
                  timer_freq);
}

void arch_timer_disable_sched(void) {
    /* Disable timer */
    write_cntp_ctl(0);
}
