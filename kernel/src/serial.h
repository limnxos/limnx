#ifndef LIMNX_SERIAL_H
#define LIMNX_SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define COM1 0x3F8

void serial_init(void);
void serial_putc(char c);
char serial_getchar(void);   /* non-blocking: returns 0 if no data */
void serial_puts(const char *s);
void serial_printf(const char *fmt, ...);

#endif
