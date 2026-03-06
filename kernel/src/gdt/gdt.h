#ifndef LIMNX_GDT_H
#define LIMNX_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_CODE   0x23   /* 0x20 | RPL 3 */
#define GDT_TSS         0x28

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void gdt_init(void);

/* Get pointer to GDT table (for TSS descriptor installation) */
struct gdt_entry *gdt_get_table(void);

/* Defined in isr_stubs.asm */
extern void gdt_flush(uint64_t gdt_ptr_addr);

#endif
