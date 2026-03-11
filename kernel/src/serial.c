#include "serial.h"
#include "io.h"
#include "fb/fbcon.h"
#include "sync/spinlock.h"
#include <stdarg.h>

static spinlock_t serial_lock = SPINLOCK_INIT;

void serial_init(void) {
    outb(COM1 + 1, 0x00);  /* Disable interrupts */
    outb(COM1 + 3, 0x80);  /* Enable DLAB */
    outb(COM1 + 0, 0x01);  /* Baud 115200 (divisor 1) */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);  /* 8N1 */
    outb(COM1 + 2, 0xC7);  /* Enable FIFO */
    outb(COM1 + 4, 0x0B);  /* IRQs enabled, RTS/DSR set */
}

static int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!is_transmit_empty())
        ;
    outb(COM1, c);
    if (fbcon_serial_on())
        fbcon_putc(c);
}

char serial_getchar(void) {
    if (inb(COM1 + 5) & 0x01)
        return (char)inb(COM1);
    return 0;
}

/* Internal unlocked puts (used by serial_printf) */
static void serial_puts_unlocked(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puts(const char *s) {
    uint64_t flags;
    spin_lock_irqsave(&serial_lock, &flags);
    serial_puts_unlocked(s);
    spin_unlock_irqrestore(&serial_lock, flags);
}

static void print_uint64(uint64_t val) {
    char buf[21];
    int i = 0;
    if (val == 0) {
        serial_putc('0');
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    while (--i >= 0)
        serial_putc(buf[i]);
}

static void print_int64(int64_t val) {
    if (val < 0) {
        serial_putc('-');
        print_uint64((uint64_t)0 - (uint64_t)val);
    } else {
        print_uint64((uint64_t)val);
    }
}

static void print_hex(uint64_t val) {
    const char *hex = "0123456789abcdef";
    serial_putc('0'); serial_putc('x');
    /* Skip leading zeros but always print at least one digit */
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int nibble = (val >> i) & 0xF;
        if (nibble || started || i == 0) {
            serial_putc(hex[nibble]);
            started = 1;
        }
    }
}

void serial_printf(const char *fmt, ...) {
    uint64_t flags;
    spin_lock_irqsave(&serial_lock, &flags);

    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                serial_putc('\r');
            serial_putc(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Handle 'l' prefix */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            serial_puts_unlocked(s ? s : "(null)");
            break;
        }
        case 'd':
            if (is_long)
                print_int64(va_arg(ap, int64_t));
            else
                print_int64((int64_t)va_arg(ap, int));
            break;
        case 'u':
            if (is_long)
                print_uint64(va_arg(ap, uint64_t));
            else
                print_uint64((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'x':
            if (is_long)
                print_hex(va_arg(ap, uint64_t));
            else
                print_hex((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'p':
            print_hex((uint64_t)va_arg(ap, void *));
            break;
        case '%':
            serial_putc('%');
            break;
        case 'c':
            serial_putc((char)va_arg(ap, int));
            break;
        default:
            serial_putc('%');
            serial_putc(*fmt);
            break;
        }
        fmt++;
    }

    va_end(ap);

    spin_unlock_irqrestore(&serial_lock, flags);
}
