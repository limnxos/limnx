#ifndef LIMNX_ARCH_INTERRUPT_H
#define LIMNX_ARCH_INTERRUPT_H

/*
 * Architecture-independent interrupt interface.
 * x86_64: IDT + 8259 PIC + LAPIC
 * ARM64:  VBAR_EL1 + GICv2
 *
 * irq_handler_t is defined per-arch (e.g., in arch/x86_64/idt.h).
 * Callers that need to register handlers include the arch-specific header.
 * This header provides only the arch_* function declarations.
 */

#include <stdint.h>

/* Forward declare — the full type is arch-defined */
struct interrupt_frame;
typedef void (*irq_handler_t)(struct interrupt_frame *frame);

void arch_interrupt_init(void);
void arch_irq_register(uint8_t irq, irq_handler_t handler);
void arch_irq_unmask(uint8_t irq);

/* Keyboard input */
char arch_kbd_getchar(void);

#endif
