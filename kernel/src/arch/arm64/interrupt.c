/*
 * ARM64 interrupt subsystem — GIC-based IRQ registration
 *
 * Corresponds to x86_64 idt.c (HAL wrappers).
 * Uses GICv2 for interrupt management, VBAR_EL1 for exception vectors.
 */

#include "arch/interrupt.h"
#include "arch/arm64/gic.h"
#include "arch/serial.h"

/* IRQ handler table — up to 256 handlers */
#define MAX_IRQ_HANDLERS 256
static irq_handler_t irq_handlers[MAX_IRQ_HANDLERS];

/* External: exception vector table defined in vectors.S */
extern char arm64_vectors[];

void arch_interrupt_init(void) {
    /* Install exception vector table */
    uint64_t vbar = (uint64_t)arm64_vectors;
    __asm__ volatile ("msr vbar_el1, %0" : : "r"(vbar));
    __asm__ volatile ("isb");

    /* Initialize GIC */
    gic_init();

    serial_puts("[irq]  ARM64 interrupt init complete (VBAR + GICv2)\n");
}

void arch_irq_register(uint8_t irq, irq_handler_t handler) {
    irq_handlers[irq] = handler;
}

void arch_irq_unmask(uint8_t irq) {
    /* Map to GIC SPI space: IRQ 0-15 → GIC SPI 32-47 (same as x86 PIC mapping) */
    gic_enable_irq(irq + GIC_SPI_START);
}

char arch_kbd_getchar(void) {
    /* ARM64: keyboard input via PL011 UART RX */
    return serial_getchar();
}
