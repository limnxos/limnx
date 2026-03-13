/*
 * ARM64 GICv2 (Generic Interrupt Controller) — stub header
 *
 * Corresponds to x86_64 LAPIC. QEMU virt machine provides GICv2 at:
 *   GIC Distributor: 0x08000000
 *   GIC CPU Interface: 0x08010000
 */

#ifndef LIMNX_ARM64_GIC_H
#define LIMNX_ARM64_GIC_H

#include <stdint.h>

/* GICv2 base addresses (QEMU virt) */
#define GICD_BASE   0x08000000ULL
#define GICC_BASE   0x08010000ULL

/* Distributor registers */
#define GICD_CTLR       0x000
#define GICD_TYPER      0x004
#define GICD_ISENABLER  0x100
#define GICD_ICENABLER  0x180
#define GICD_ISPENDR    0x200
#define GICD_ICPENDR    0x280
#define GICD_IPRIORITYR 0x400
#define GICD_ITARGETSR  0x800
#define GICD_ICFGR      0xC00

/* CPU interface registers */
#define GICC_CTLR       0x000
#define GICC_PMR        0x004
#define GICC_IAR        0x00C
#define GICC_EOIR       0x010

/* Interrupt types */
#define GIC_SGI_MAX     16      /* Software Generated Interrupts: 0-15 */
#define GIC_PPI_START   16      /* Private Peripheral Interrupts: 16-31 */
#define GIC_SPI_START   32      /* Shared Peripheral Interrupts: 32+ */

/* ARM64 generic timer PPI IDs */
#define GIC_TIMER_NS_EL1    30  /* Non-secure EL1 physical timer */
#define GIC_TIMER_NS_EL2    26  /* Non-secure EL2 physical timer */
#define GIC_TIMER_VIRT      27  /* Virtual timer */

void     gic_init(void);
void     gic_cpu_interface_init(void);   /* per-CPU init for AP cores */
void     gic_eoi(uint32_t irq);
uint32_t gic_ack(void);
void     gic_enable_irq(uint32_t irq);
void     gic_disable_irq(uint32_t irq);
void     gic_set_priority(uint32_t irq, uint8_t priority);
void     gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id);

#endif
