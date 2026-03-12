#ifndef LIMNX_X86_64_LAPIC_H
#define LIMNX_X86_64_LAPIC_H

#include <stdint.h>

/* LAPIC register offsets (memory-mapped, relative to base 0xFEE00000) */
#define LAPIC_ID        0x020
#define LAPIC_VERSION   0x030
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_ICR 0x380
#define LAPIC_TIMER_CCR 0x390
#define LAPIC_TIMER_DCR 0x3E0

/* SVR bits */
#define LAPIC_SVR_ENABLE 0x100

/* Timer modes */
#define LAPIC_TIMER_PERIODIC  0x20000
#define LAPIC_TIMER_MASKED    0x10000

/* LAPIC timer vector */
#define LAPIC_TIMER_VECTOR 48

/* TLB shootdown IPI vector */
#define LAPIC_TLB_VECTOR   49

/* Spurious vector */
#define LAPIC_SPURIOUS_VECTOR 255

/* LAPIC physical base */
#define LAPIC_PHYS_BASE 0xFEE00000ULL

void     lapic_init(void);
void     lapic_eoi(void);
uint32_t lapic_get_id(void);
void     lapic_timer_calibrate(void);
void     lapic_timer_start(uint32_t ms);
void     lapic_timer_stop(void);
void     lapic_send_ipi(uint32_t target_lapic_id, uint32_t vector);

#endif
