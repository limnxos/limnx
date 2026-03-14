/*
 * ARM64 GICv2 driver — stub implementation
 *
 * Corresponds to x86_64 lapic.c. Provides GIC distributor and CPU
 * interface initialization, EOI, IRQ enable/disable, and SGI (IPI).
 *
 * QEMU virt machine GICv2 addresses:
 *   Distributor:   0x08000000
 *   CPU Interface: 0x08010000
 */

#include "arch/arm64/gic.h"
#include "arch/serial.h"
#include "dtb/dtb.h"

/* Runtime GIC base addresses (default: QEMU virt) */
uint64_t gicd_base_addr = 0x08000000ULL;
uint64_t gicc_base_addr = 0x08010000ULL;

static volatile uint32_t *gicd_base;
static volatile uint32_t *gicc_base;

static inline uint32_t gicd_read(uint32_t reg) {
    return gicd_base[reg / 4];
}

static inline void gicd_write(uint32_t reg, uint32_t val) {
    gicd_base[reg / 4] = val;
}

static inline uint32_t gicc_read(uint32_t reg) {
    return gicc_base[reg / 4];
}

static inline void gicc_write(uint32_t reg, uint32_t val) {
    gicc_base[reg / 4] = val;
}

void gic_init(void) {
    /* Use DTB-discovered addresses if available */
    const dtb_platform_info_t *plat = dtb_get_platform();
    if (plat && plat->valid) {
        gicd_base_addr = plat->gic_dist_base;
        gicc_base_addr = plat->gic_cpu_base;
    }
    gicd_base = (volatile uint32_t *)gicd_base_addr;
    gicc_base = (volatile uint32_t *)gicc_base_addr;

    /* Disable distributor during setup */
    gicd_write(GICD_CTLR, 0);

    /* Find out how many IRQ lines are supported */
    uint32_t typer = gicd_read(GICD_TYPER);
    uint32_t irq_lines = ((typer & 0x1F) + 1) * 32;
    (void)irq_lines;

    /* Disable all IRQs */
    for (uint32_t i = 0; i < irq_lines / 32; i++)
        gicd_write(GICD_ICENABLER + i * 4, 0xFFFFFFFF);

    /* Set all IRQs to lowest priority */
    for (uint32_t i = 0; i < irq_lines / 4; i++)
        gicd_write(GICD_IPRIORITYR + i * 4, 0xA0A0A0A0);

    /* Route all SPIs to CPU 0 */
    for (uint32_t i = GIC_SPI_START / 4; i < irq_lines / 4; i++)
        gicd_write(GICD_ITARGETSR + i * 4, 0x01010101);

    /* Configure all SPIs as level-triggered */
    for (uint32_t i = GIC_SPI_START / 16; i < irq_lines / 16; i++)
        gicd_write(GICD_ICFGR + i * 4, 0);

    /* Enable distributor */
    gicd_write(GICD_CTLR, 1);

    /* CPU interface: set priority mask to accept all */
    gicc_write(GICC_PMR, 0xFF);

    /* Enable CPU interface */
    gicc_write(GICC_CTLR, 1);

    serial_puts("[gic]  GICv2 initialized\n");
}

void gic_eoi(uint32_t irq) {
    gicc_write(GICC_EOIR, irq);
}

uint32_t gic_ack(void) {
    return gicc_read(GICC_IAR) & 0x3FF;
}

void gic_enable_irq(uint32_t irq) {
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    gicd_write(GICD_ISENABLER + reg * 4, 1U << bit);
}

void gic_disable_irq(uint32_t irq) {
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    gicd_write(GICD_ICENABLER + reg * 4, 1U << bit);
}

void gic_set_priority(uint32_t irq, uint8_t priority) {
    uint32_t reg = irq / 4;
    uint32_t shift = (irq % 4) * 8;
    uint32_t val = gicd_read(GICD_IPRIORITYR + reg * 4);
    val &= ~(0xFFU << shift);
    val |= ((uint32_t)priority << shift);
    gicd_write(GICD_IPRIORITYR + reg * 4, val);
}

void gic_cpu_interface_init(void) {
    /*
     * Per-CPU GIC CPU interface initialization.
     * Called by each AP during SMP bringup.
     * The GIC CPU interface is banked per-CPU at the same MMIO address,
     * so each core's writes go to its own CPU interface.
     */

    /* Set priority mask to accept all interrupts */
    gicc_write(GICC_PMR, 0xFF);

    /* Enable CPU interface */
    gicc_write(GICC_CTLR, 1);
}

void gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id) {
    /* SGI: target list filter = 0 (use target list), target = cpu bit */
    uint32_t val = (1U << (16 + target_cpu)) | (sgi_id & 0xF);
    gicd_write(0xF00, val);  /* GICD_SGIR */
}
