#ifndef LIMNX_ARCH_TIMER_H
#define LIMNX_ARCH_TIMER_H

/*
 * Architecture-independent timer / timekeeping interface.
 * x86_64: PIT tick counter + LAPIC periodic timer
 * ARM64:  Generic Timer (CNTPCT_EL0)
 */

#include <stdint.h>

uint64_t arch_timer_get_ticks(void);
void     arch_timer_enable_sched(void);
void     arch_timer_disable_sched(void);

#endif
