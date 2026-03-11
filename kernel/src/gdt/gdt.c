#define pr_fmt(fmt) "[gdt]  " fmt
#include "klog.h"

#include "gdt/gdt.h"
#include "serial.h"

static struct gdt_entry gdt[7]; /* 5 standard + 2 for TSS (16-byte descriptor) */
static struct gdt_ptr   gdtp;

static void gdt_set_entry(int i, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[i].base_low    = base & 0xFFFF;
    gdt[i].base_mid    = (base >> 16) & 0xFF;
    gdt[i].base_high   = (base >> 24) & 0xFF;
    gdt[i].limit_low   = limit & 0xFFFF;
    gdt[i].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].access      = access;
}

void gdt_init(void) {
    /* Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* Kernel code: present, DPL 0, code, readable, long mode (L=1) */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);

    /* Kernel data: present, DPL 0, data, writable */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);

    /* User data: present, DPL 3, data, writable (must be before user code for SYSRET) */
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    /* User code: present, DPL 3, code, readable, long mode (L=1) */
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);

    /* Slots 5-6 reserved for TSS (installed by tss_init) */
    gdt_set_entry(5, 0, 0, 0, 0);
    gdt_set_entry(6, 0, 0, 0, 0);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)&gdt;

    gdt_flush((uint64_t)&gdtp);

    pr_info("GDT loaded\n");
}

struct gdt_entry *gdt_get_table(void) {
    return gdt;
}
