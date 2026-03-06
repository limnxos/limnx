#ifndef LIMNX_FBCON_H
#define LIMNX_FBCON_H

#include <stdint.h>

void fbcon_init(void *fb_addr, uint64_t width, uint64_t height,
                uint64_t pitch, uint32_t bpp);
void fbcon_putc(char c);
void fbcon_clear(void);
int  fbcon_active(void);       /* returns 1 if initialized */
void fbcon_set_serial(int on); /* enable/disable serial_putc hook */
int  fbcon_serial_on(void);    /* returns 1 if serial hook is enabled */

#endif
