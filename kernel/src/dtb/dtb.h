#ifndef LIMNX_DTB_H
#define LIMNX_DTB_H

/*
 * Flattened Device Tree (FDT/DTB) parser.
 *
 * Architecture-neutral parser for the DTB binary format.
 * QEMU passes a DTB pointer at boot; this module extracts
 * hardware addresses (RAM, GIC, UART, virtio-mmio) to replace
 * hardcoded constants.
 */

#include <stdint.h>

/* Cached platform hardware info */
typedef struct {
    uint64_t ram_base;
    uint64_t ram_size;
    uint64_t gic_dist_base;
    uint64_t gic_cpu_base;
    uint64_t uart_base;
    uint64_t virtio_mmio_base;     /* lowest virtio-mmio slot address */
    uint32_t virtio_mmio_num_slots;
    uint32_t virtio_mmio_slot_size;
    int      valid;                /* 1 if dtb_init succeeded */
} dtb_platform_info_t;

/* Parse DTB and populate platform info. Returns 0 on success. */
int dtb_init(void *dtb_ptr);

/* Get cached platform info (valid after dtb_init) */
const dtb_platform_info_t *dtb_get_platform(void);

#endif
