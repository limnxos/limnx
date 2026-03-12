#ifndef LIMNX_X86_64_IDT_H
#define LIMNX_X86_64_IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* Interrupt stack frame pushed by CPU + our stubs */
typedef struct interrupt_frame {
    /* Pushed by isr_common_stub */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* Pushed by stub */
    uint64_t int_no, err_code;
    /* Pushed by CPU */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) interrupt_frame_t;

typedef void (*irq_handler_t)(interrupt_frame_t *frame);

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init(void);
void idt_set_lapic_timer_active(void);

/* Internal — used by arch/x86_64/smp.c and boot.c */
uint64_t pit_get_ticks(void);
void pit_enable_sched(void);
void pit_disable_sched(void);
void irq_register_handler(uint8_t irq, irq_handler_t handler);
void irq_unmask(uint8_t irq);
char kbd_getchar(void);

#endif
