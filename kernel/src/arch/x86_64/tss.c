#define pr_fmt(fmt) "[tss]  " fmt
#include "klog.h"

#include "arch/x86_64/tss.h"
#include "arch/x86_64/gdt.h"

static tss_t tss;

/*
 * In long mode, the TSS descriptor is 16 bytes (2 GDT slots).
 * Format:
 *   Slot N:   limit[15:0] | base[15:0] | base[23:16] | type/attr | limit[19:16]/flags | base[31:24]
 *   Slot N+1: base[63:32] | reserved
 */
static void install_tss_descriptor(void) {
    uint64_t base = (uint64_t)&tss;
    uint32_t limit = sizeof(tss_t) - 1;

    /* Access byte: present=1, DPL=0, type=0x9 (64-bit TSS available) */
    uint8_t access = 0x89;
    /* Granularity: byte granularity, no flags needed */
    uint8_t gran = 0x00;

    /* Install into GDT slots 5 and 6 (selector 0x28) */
    struct gdt_entry *gdt = gdt_get_table();

    /* Slot 5: low 8 bytes of TSS descriptor */
    gdt[5].limit_low   = limit & 0xFFFF;
    gdt[5].base_low    = base & 0xFFFF;
    gdt[5].base_mid    = (base >> 16) & 0xFF;
    gdt[5].access      = access;
    gdt[5].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[5].base_high   = (base >> 24) & 0xFF;

    /* Slot 6: high 8 bytes — base[63:32] + reserved */
    uint32_t *slot6 = (uint32_t *)&gdt[6];
    slot6[0] = (uint32_t)(base >> 32);
    slot6[1] = 0;
}

void tss_init(void) {
    /* Zero the TSS */
    uint8_t *p = (uint8_t *)&tss;
    for (uint64_t i = 0; i < sizeof(tss_t); i++)
        p[i] = 0;

    tss.iopb_offset = sizeof(tss_t);

    install_tss_descriptor();

    /* Load the task register */
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS_SELECTOR));

    pr_info("TSS loaded\n");
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
