#define pr_fmt(fmt) "[idt]  " fmt
#include "klog.h"

#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/lapic.h"
#include "arch/percpu.h"
#include "sched/sched.h"
#include "pty/pty.h"
#include "arch/cpu.h"
#include "arch/paging.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

static volatile uint64_t pit_ticks = 0;
static volatile int sched_enabled = 0;
static volatile int lapic_timer_active = 0;  /* set when LAPIC drives scheduling */

/* --- Keyboard ring buffer --- */
#define KBD_BUF_SIZE 256
static volatile char kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_read_pos = 0;
static volatile uint32_t kbd_write_pos = 0;

/* PS/2 Set 1 scancode-to-ASCII (unshifted only) */
static const char scancode_to_ascii[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};

/* Dynamic IRQ handler table (IRQ 0-15, up to 4 handlers per IRQ for sharing) */
#define IRQ_MAX_HANDLERS 4
static irq_handler_t irq_handlers[16][IRQ_MAX_HANDLERS];

/* ISR stub table defined in isr_stubs.asm (48 legacy + LAPIC timer + spurious) */
extern void *isr_stub_table[50];

static const char *exception_names[] = {
    "Division Error",           /*  0 */
    "Debug",                    /*  1 */
    "NMI",                      /*  2 */
    "Breakpoint",               /*  3 */
    "Overflow",                 /*  4 */
    "Bound Range Exceeded",     /*  5 */
    "Invalid Opcode",           /*  6 */
    "Device Not Available",     /*  7 */
    "Double Fault",             /*  8 */
    "Coprocessor Segment",      /*  9 */
    "Invalid TSS",              /* 10 */
    "Segment Not Present",      /* 11 */
    "Stack-Segment Fault",      /* 12 */
    "General Protection Fault", /* 13 */
    "Page Fault",               /* 14 */
    "Reserved",                 /* 15 */
    "x87 FP Exception",        /* 16 */
    "Alignment Check",          /* 17 */
    "Machine Check",            /* 18 */
    "SIMD FP Exception",       /* 19 */
    "Virtualization Exception", /* 20 */
    "Control Protection",       /* 21 */
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",     /* 28 */
    "VMM Communication",        /* 29 */
    "Security Exception",       /* 30 */
    "Reserved",                 /* 31 */
};

static void idt_set_entry(int i, uint64_t handler, uint16_t selector,
                           uint8_t type_attr) {
    idt[i].offset_low  = handler & 0xFFFF;
    idt[i].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[i].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[i].selector    = selector;
    idt[i].ist         = 0;
    idt[i].type_attr   = type_attr;
    idt[i].reserved    = 0;
}

static void pic_remap(void) {
    /* Save masks */
    uint8_t mask1 = inb(0x21);
    uint8_t mask2 = inb(0xA1);

    /* ICW1: begin init, expect ICW4 */
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();

    /* ICW2: vector offsets */
    outb(0x21, 0x20); io_wait();  /* Master: IRQ 0-7 → ISR 32-39 */
    outb(0xA1, 0x28); io_wait();  /* Slave:  IRQ 8-15 → ISR 40-47 */

    /* ICW3: cascading */
    outb(0x21, 0x04); io_wait();  /* Master: slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();  /* Slave:  cascade identity 2 */

    /* ICW4: 8086 mode */
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    /* Restore saved masks */
    outb(0x21, mask1);
    outb(0xA1, mask2);
}

static void pic_set_mask(void) {
    /* Unmask IRQ0 (PIT) and IRQ1 (keyboard) only */
    /* Master: mask all except bit 0 (IRQ0) and bit 1 (IRQ1) → 0xFC */
    outb(0x21, 0xFC);
    /* Slave: mask all → 0xFF */
    outb(0xA1, 0xFF);
}

static void dump_regs(interrupt_frame_t *frame) {
    serial_printf("  RAX=%lx RBX=%lx RCX=%lx RDX=%lx\n",
        frame->rax, frame->rbx, frame->rcx, frame->rdx);
    serial_printf("  RSI=%lx RDI=%lx RBP=%lx RSP=%lx\n",
        frame->rsi, frame->rdi, frame->rbp, frame->rsp);
    serial_printf("  R8 =%lx R9 =%lx R10=%lx R11=%lx\n",
        frame->r8, frame->r9, frame->r10, frame->r11);
    serial_printf("  R12=%lx R13=%lx R14=%lx R15=%lx\n",
        frame->r12, frame->r13, frame->r14, frame->r15);
    serial_printf("  RIP=%lx CS=%lx RFLAGS=%lx ERR=%lx\n",
        frame->rip, frame->cs, frame->rflags, frame->err_code);
}

/* Called from isr_common_stub in asm */
void interrupt_dispatch(interrupt_frame_t *frame) {
    uint64_t vec = frame->int_no;

    if (vec < 32) {
        /* Page Fault — dispatch to handler */
        if (vec == 14) {
            uint64_t cr2 = arch_get_fault_address();
            uint64_t err = frame->err_code;
            extern int page_fault_handler(uint64_t fault_addr, uint64_t err_code,
                                          interrupt_frame_t *frame);
            if (page_fault_handler(cr2, err, frame) == 0)
                return;  /* Handled — resume */
            /* Unhandled — fall through to fatal */
            serial_printf("\n!!! EXCEPTION %lu: %s !!!\n",
                vec, exception_names[vec]);
            dump_regs(frame);
            serial_printf("  CR2=%lx (fault address)\n", cr2);
            serial_puts("  HALTING.\n");
            arch_irq_disable();
            for (;;)
                arch_halt();
        }

        /* CPU exception */
        serial_printf("\n!!! EXCEPTION %lu: %s !!!\n",
            vec, exception_names[vec]);
        dump_regs(frame);

        if (vec == 3) {
            /* Breakpoint — resumable */
            serial_puts("  (breakpoint — resuming)\n");
            return;
        }

        /* Fatal exception — halt */
        serial_puts("  HALTING.\n");
        arch_irq_disable();
        for (;;)
            arch_halt();
    }

    if (vec >= 32 && vec < 48) {
        /* Hardware IRQ */
        uint8_t irq = vec - 32;

        if (irq == 0) {
            /* PIT timer tick — keep counting for timekeeping */
            pit_ticks++;

            /* EOI must be sent before schedule() to allow nested IRQs */
            outb(0x20, 0x20);

            /* Only drive scheduling from PIT if LAPIC timer is not active.
             * With SMP, LAPIC timers handle scheduling on all CPUs. */
            if (sched_enabled && !lapic_timer_active)
                sched_tick();
            return;
        } else if (irq == 1) {
            /* Keyboard — read scancode and push ASCII to ring buffer */
            uint8_t scancode = inb(0x60);
            if (!(scancode & 0x80) && scancode < 128) {
                char ch = scancode_to_ascii[scancode];
                if (ch) {
                    /* Feed to console PTY if active */
                    if (pty_get_console() >= 0) {
                        pty_console_input(ch);
                    }
                    /* Also keep in keyboard ring buffer for sys_getchar */
                    uint32_t next = (kbd_write_pos + 1) % KBD_BUF_SIZE;
                    if (next != kbd_read_pos) {
                        kbd_buf[kbd_write_pos] = ch;
                        kbd_write_pos = next;
                    }
                }
            }
        } else {
            /* Dynamic handlers (may be shared) */
            for (int h = 0; h < IRQ_MAX_HANDLERS; h++) {
                if (irq_handlers[irq][h])
                    irq_handlers[irq][h](frame);
            }
        }

        /* Send EOI */
        if (irq >= 8)
            outb(0xA0, 0x20); /* Slave EOI */
        outb(0x20, 0x20);     /* Master EOI */
    }

    /* LAPIC timer vector */
    if (vec == 48) {
        lapic_eoi();
        if (sched_enabled)
            sched_tick();
        return;
    }

    /* TLB shootdown IPI vector */
    if (vec == 49) {
        /* Reload CR3 to flush entire TLB on this CPU */
        arch_switch_address_space(arch_get_address_space());
        lapic_eoi();
        return;
    }

    /* LAPIC spurious vector — just return */
    if (vec == 255) {
        return;
    }
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

void pit_enable_sched(void) {
    sched_enabled = 1;
}

void pit_disable_sched(void) {
    sched_enabled = 0;
}

void idt_set_lapic_timer_active(void) {
    lapic_timer_active = 1;
}

void irq_register_handler(uint8_t irq, irq_handler_t handler) {
    if (irq >= 16) return;
    for (int i = 0; i < IRQ_MAX_HANDLERS; i++) {
        if (irq_handlers[irq][i] == 0) {
            irq_handlers[irq][i] = handler;
            return;
        }
    }
}

void irq_unmask(uint8_t irq) {
    if (irq < 8) {
        uint8_t mask = inb(0x21);
        mask &= ~(1 << irq);
        outb(0x21, mask);
    } else if (irq < 16) {
        /* Unmask on slave PIC */
        uint8_t mask = inb(0xA1);
        mask &= ~(1 << (irq - 8));
        outb(0xA1, mask);
        /* Also unmask IRQ2 (cascade) on master */
        uint8_t master = inb(0x21);
        master &= ~(1 << 2);
        outb(0x21, master);
    }
}

char kbd_getchar(void) {
    if (kbd_read_pos == kbd_write_pos)
        return 0;
    char ch = kbd_buf[kbd_read_pos];
    kbd_read_pos = (kbd_read_pos + 1) % KBD_BUF_SIZE;
    return ch;
}

void idt_init(void) {
    /* Remap PIC first */
    pic_remap();
    pic_set_mask();

    /* Install 48 handlers (exceptions 0-31 + IRQs 0-15) */
    for (int i = 0; i < 48; i++) {
        /* 0x8E = present, DPL 0, 64-bit interrupt gate */
        idt_set_entry(i, (uint64_t)isr_stub_table[i], GDT_KERNEL_CODE, 0x8E);
    }

    /* LAPIC timer vector 48 */
    idt_set_entry(48, (uint64_t)isr_stub_table[48], GDT_KERNEL_CODE, 0x8E);

    /* TLB shootdown IPI vector 49 */
    idt_set_entry(49, (uint64_t)isr_stub_table[49], GDT_KERNEL_CODE, 0x8E);

    /* Zero remaining entries */
    for (int i = 50; i < IDT_ENTRIES; i++) {
        idt_set_entry(i, 0, 0, 0);
    }

    /* LAPIC spurious vector 255 */
    idt_set_entry(255, (uint64_t)isr_stub_table[49], GDT_KERNEL_CODE, 0x8E);

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)&idt;

    __asm__ volatile ("lidt %0" : : "m"(idtp));

    pr_info("IDT loaded (48 + LAPIC timer/spurious vectors)\n");
    pr_info("PIC remapped: IRQ0-7 → 32-39, IRQ8-15 → 40-47\n");

    /* Enable interrupts */
    arch_irq_enable();
    pr_info("Interrupts enabled\n");
}

/* HAL wrappers — implement arch_* interfaces */
uint64_t arch_timer_get_ticks(void) { return pit_get_ticks(); }
void arch_timer_enable_sched(void) { pit_enable_sched(); }
void arch_timer_disable_sched(void) { pit_disable_sched(); }
char arch_kbd_getchar(void) { return kbd_getchar(); }
void arch_irq_register(uint8_t irq, irq_handler_t handler) { irq_register_handler(irq, handler); }
void arch_irq_unmask(uint8_t irq) { irq_unmask(irq); }
void arch_interrupt_init(void) { idt_init(); }
