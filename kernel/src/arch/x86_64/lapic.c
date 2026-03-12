#define pr_fmt(fmt) "[lapic] " fmt
#include "klog.h"

#include "arch/x86_64/lapic.h"
#include "arch/percpu.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "arch/x86_64/io.h"
#include "arch/cpu.h"

/* LAPIC base virtual address (HHDM-mapped) */
static volatile uint32_t *lapic_base;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic_base[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic_base[reg / 4] = val;
}

static int lapic_mapped = 0;

void lapic_init(void) {
    extern uint64_t hhdm_offset;
    lapic_base = (volatile uint32_t *)(LAPIC_PHYS_BASE + hhdm_offset);

    /* Map the LAPIC MMIO page once (not covered by HHDM for regular RAM).
     * Use PTE_WRITABLE | PTE_NX | PTE_PCD (cache disable) for MMIO. */
    if (!lapic_mapped) {
        vmm_map_page(LAPIC_PHYS_BASE + hhdm_offset, LAPIC_PHYS_BASE,
                     PTE_WRITABLE | PTE_NX | (1ULL << 4) /* PCD */);
        lapic_mapped = 1;
    }

    /* Enable LAPIC via SVR: set enable bit + spurious vector */
    uint32_t svr = lapic_read(LAPIC_SVR);
    svr |= LAPIC_SVR_ENABLE | LAPIC_SPURIOUS_VECTOR;
    lapic_write(LAPIC_SVR, svr);

    /* Set task priority to 0 (accept all interrupts) */
    lapic_write(LAPIC_TPR, 0);

    pr_info("LAPIC enabled (ID=%u)\n", lapic_get_id());
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

/*
 * Calibrate LAPIC timer using PIT channel 2 as reference.
 * We count how many LAPIC ticks occur in ~10ms.
 */
void lapic_timer_calibrate(void) {
    /* Set LAPIC timer divider to 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);  /* divide by 16 */

    /* Configure PIT channel 2 for one-shot ~10ms */
    /* PIT frequency = 1193182 Hz, 10ms = 11932 counts */
    uint16_t pit_count = 11932;

    /* Gate channel 2 on (bit 0 of port 0x61) */
    uint8_t port61 = inb(0x61);
    outb(0x61, (port61 & 0xFD) | 0x01);  /* enable gate, clear output */

    /* Configure PIT channel 2: mode 0 (one-shot), lobyte/hibyte */
    outb(0x43, 0xB0);  /* channel 2, lobyte/hibyte, mode 0, binary */
    outb(0x42, (uint8_t)(pit_count & 0xFF));
    outb(0x42, (uint8_t)(pit_count >> 8));

    /* Reset PIT gate to start countdown */
    outb(0x61, inb(0x61) & 0xFE);
    outb(0x61, inb(0x61) | 0x01);

    /* Start LAPIC timer with max count */
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    /* Wait for PIT to fire (bit 5 of port 0x61 goes high) */
    while (!(inb(0x61) & 0x20))
        ;

    /* Stop LAPIC timer */
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED);

    uint32_t elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);

    /* elapsed ticks in ~10ms → ticks per ms = elapsed / 10 */
    percpu_t *self = percpu_get();
    self->lapic_ticks_per_ms = elapsed / 10;

    pr_info("Timer calibrated: %u ticks/ms\n",
                  self->lapic_ticks_per_ms);
}

void lapic_timer_start(uint32_t ms) {
    percpu_t *self = percpu_get();
    uint32_t count = self->lapic_ticks_per_ms * ms;

    /* Divider = 16 */
    lapic_write(LAPIC_TIMER_DCR, 0x03);

    /* Periodic mode, vector = LAPIC_TIMER_VECTOR */
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VECTOR);

    /* Start */
    lapic_write(LAPIC_TIMER_ICR, count);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_TIMER_LVT, LAPIC_TIMER_MASKED);
}

void lapic_send_ipi(uint32_t target_lapic_id, uint32_t vector) {
    lapic_write(LAPIC_ICR_HI, target_lapic_id << 24);
    lapic_write(LAPIC_ICR_LO, vector);

    /* Wait for delivery */
    while (lapic_read(LAPIC_ICR_LO) & (1 << 12))
        arch_pause();
}
