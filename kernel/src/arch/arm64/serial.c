/*
 * PL011 UART driver for ARM64 QEMU virt machine.
 * UART base address: 0x09000000
 */

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include "arch/serial.h"

/* PL011 register offsets */
#define UART_BASE   0x09000000ULL
#define UART_DR     (*(volatile uint32_t *)(UART_BASE + 0x000))  /* Data Register */
#define UART_FR     (*(volatile uint32_t *)(UART_BASE + 0x018))  /* Flag Register */
#define UART_IBRD   (*(volatile uint32_t *)(UART_BASE + 0x024))  /* Integer Baud Rate */
#define UART_FBRD   (*(volatile uint32_t *)(UART_BASE + 0x028))  /* Fractional Baud Rate */
#define UART_LCR_H  (*(volatile uint32_t *)(UART_BASE + 0x02C))  /* Line Control */
#define UART_CR     (*(volatile uint32_t *)(UART_BASE + 0x030))  /* Control Register */
#define UART_IMSC   (*(volatile uint32_t *)(UART_BASE + 0x038))  /* Interrupt Mask */
#define UART_ICR    (*(volatile uint32_t *)(UART_BASE + 0x044))  /* Interrupt Clear */

/* Flag register bits */
#define FR_TXFF     (1 << 5)  /* TX FIFO full */
#define FR_RXFE     (1 << 4)  /* RX FIFO empty */

void serial_init(void) {
    /* Disable UART */
    UART_CR = 0;

    /* Clear all interrupts */
    UART_ICR = 0x7FF;

    /* Set baud rate: 115200 at 24MHz UARTCLK (QEMU default)
     * IBRD = 24000000 / (16 * 115200) = 13
     * FBRD = round(0.02 * 64) = 1 */
    UART_IBRD = 13;
    UART_FBRD = 1;

    /* 8N1, enable FIFOs */
    UART_LCR_H = (3 << 5) | (1 << 4);  /* WLEN=8, FEN=1 */

    /* Mask all interrupts */
    UART_IMSC = 0;

    /* Enable UART, TX, RX */
    UART_CR = (1 << 0) | (1 << 8) | (1 << 9);  /* UARTEN, TXE, RXE */
}

void serial_putc(char c) {
    /* Wait until TX FIFO is not full */
    while (UART_FR & FR_TXFF)
        ;
    UART_DR = (uint32_t)c;
}

char serial_getchar(void) {
    if (UART_FR & FR_RXFE)
        return 0;  /* No data available */
    return (char)(UART_DR & 0xFF);
}

static void serial_puts_unlocked(const char *s) {
    while (*s) {
        if (*s == '\n')
            serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_puts(const char *s) {
    serial_puts_unlocked(s);
}

/* Minimal printf for ARM64 boot */

static void print_uint64(uint64_t val) {
    char buf[21];
    int i = 0;
    if (val == 0) { serial_putc('0'); return; }
    while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
    while (--i >= 0) serial_putc(buf[i]);
}

static void print_int64(int64_t val) {
    if (val < 0) { serial_putc('-'); print_uint64((uint64_t)0 - (uint64_t)val); }
    else print_uint64((uint64_t)val);
}

static void print_hex(uint64_t val) {
    const char *hex = "0123456789abcdef";
    serial_putc('0'); serial_putc('x');
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
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            if (*fmt == '\n') serial_putc('\r');
            serial_putc(*fmt++);
            continue;
        }
        fmt++;
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            serial_puts_unlocked(s ? s : "(null)");
            break;
        }
        case 'd':
            if (is_long) print_int64(va_arg(ap, int64_t));
            else print_int64((int64_t)va_arg(ap, int));
            break;
        case 'u':
            if (is_long) print_uint64(va_arg(ap, uint64_t));
            else print_uint64((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'x':
            if (is_long) print_hex(va_arg(ap, uint64_t));
            else print_hex((uint64_t)va_arg(ap, unsigned int));
            break;
        case 'p':
            print_hex((uint64_t)va_arg(ap, void *));
            break;
        case '%': serial_putc('%'); break;
        case 'c': serial_putc((char)va_arg(ap, int)); break;
        default: serial_putc('%'); serial_putc(*fmt); break;
        }
        fmt++;
    }

    va_end(ap);
}
