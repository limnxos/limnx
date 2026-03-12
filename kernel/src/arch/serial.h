#ifndef LIMNX_ARCH_SERIAL_H
#define LIMNX_ARCH_SERIAL_H

/*
 * Architecture-independent serial console interface.
 * Each architecture provides a serial.c implementing these functions.
 * x86_64: COM1 UART (port I/O)
 * ARM64:  PL011 UART (MMIO)
 */

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
void serial_putc(char c);
char serial_getchar(void);
void serial_puts(const char *s);
void serial_printf(const char *fmt, ...);

#endif
