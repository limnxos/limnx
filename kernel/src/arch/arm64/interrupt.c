#include "arch/interrupt.h"
#include "arch/serial.h"

void arch_interrupt_init(void) {
    serial_puts("[irq]  ARM64 interrupt init (GIC stub)\n");
}

void arch_irq_register(uint8_t irq, irq_handler_t handler) {
    (void)irq; (void)handler;
}

void arch_irq_unmask(uint8_t irq) {
    (void)irq;
}

char arch_kbd_getchar(void) {
    return 0;
}
