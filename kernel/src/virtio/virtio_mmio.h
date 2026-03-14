/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIMNX_VIRTIO_MMIO_H
#define LIMNX_VIRTIO_MMIO_H

/*
 * Virtio-MMIO transport header.
 *
 * QEMU virt machine exposes virtio-mmio devices at:
 *   base = 0x0a000000 + 0x200 * slot   (slot 0..31)
 *
 * Each 0x200-byte region contains the virtio-mmio register set.
 * This header defines register offsets and MMIO accessors shared by the
 * ARM64 virtio-blk-mmio and virtio-net-mmio drivers.
 *
 * Virtqueue structures (virtq_desc_t, virtq_avail_t, virtq_used_t) are
 * defined in net/virtio_net.h and reused by both drivers.
 * Block request header (virtio_blk_req_t) is in blk/virtio_blk.h.
 */

#include <stdint.h>
#include "compiler.h"
#include "arch/cpu.h"

/* ------------------------------------------------------------------ */
/* MMIO register offsets (virtio-mmio legacy / v1)                    */
/* ------------------------------------------------------------------ */

#define VIRTIO_MMIO_MAGIC_VALUE        0x000  /* R   - 0x74726976 ("virt") */
#define VIRTIO_MMIO_VERSION            0x004  /* R   - 1 (legacy) or 2    */
#define VIRTIO_MMIO_DEVICE_ID          0x008  /* R   - 0=invalid,1=net,2=blk */
#define VIRTIO_MMIO_VENDOR_ID          0x00C  /* R                        */
#define VIRTIO_MMIO_DEVICE_FEATURES    0x010  /* R   - feature bits       */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014 /* W   - feature word sel   */
#define VIRTIO_MMIO_DRIVER_FEATURES    0x020  /* W   - accepted features  */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024 /* W   - feature word sel   */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE    0x028  /* W   - legacy: page size  */
#define VIRTIO_MMIO_QUEUE_SEL          0x030  /* W   - queue index        */
#define VIRTIO_MMIO_QUEUE_NUM_MAX      0x034  /* R   - max queue entries  */
#define VIRTIO_MMIO_QUEUE_NUM          0x038  /* W   - queue size to use  */
#define VIRTIO_MMIO_QUEUE_ALIGN        0x03C  /* W   - legacy: alignment  */
#define VIRTIO_MMIO_QUEUE_PFN          0x040  /* R/W - legacy: phys PFN   */
#define VIRTIO_MMIO_QUEUE_READY        0x044  /* R/W - modern only        */
#define VIRTIO_MMIO_QUEUE_NOTIFY       0x050  /* W   - queue notify       */
#define VIRTIO_MMIO_INTERRUPT_STATUS   0x060  /* R   - interrupt reason    */
#define VIRTIO_MMIO_INTERRUPT_ACK      0x064  /* W   - ack interrupts     */
#define VIRTIO_MMIO_STATUS             0x070  /* R/W - device status       */

/* Device-specific config space starts at offset 0x100 */
#define VIRTIO_MMIO_CONFIG             0x100

/* ------------------------------------------------------------------ */
/* Magic / device IDs                                                 */
/* ------------------------------------------------------------------ */

#define VIRTIO_MMIO_MAGIC              0x74726976  /* "virt" */
#define VIRTIO_MMIO_DEVID_NET          1
#define VIRTIO_MMIO_DEVID_BLK          2
#define VIRTIO_MMIO_DEVID_GPU          16

/* ------------------------------------------------------------------ */
/* MMIO accessors (volatile)                                          */
/* ------------------------------------------------------------------ */

static inline uint32_t mmio_read32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static inline uint8_t mmio_read8(uint64_t base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

/* ------------------------------------------------------------------ */
/* QEMU virt virtio-mmio slot enumeration                             */
/* ------------------------------------------------------------------ */

/* Runtime virtio-mmio parameters (set by dtb_init, defaults: QEMU virt) */
extern uint64_t virtio_mmio_base_addr;
extern uint32_t virtio_mmio_num_devices;

#define VIRTIO_MMIO_SLOT_SIZE   0x200

/* QEMU virt IRQs: SPI 16+slot  =>  GIC INTID = 32 + 16 + slot = 48+slot */
#define VIRTIO_MMIO_IRQ(slot)   (48 + (slot))

static inline uint64_t virtio_mmio_slot_base(uint32_t slot) {
    return virtio_mmio_base_addr + (uint64_t)slot * VIRTIO_MMIO_SLOT_SIZE;
}

/* ------------------------------------------------------------------ */
/* Virtqueue memory layout helpers                                    */
/* ------------------------------------------------------------------ */

/*
 * Compute total bytes needed for a virtqueue of size @qsz.
 * Layout (legacy):
 *   descriptors : qsz * 16
 *   avail ring  : 6 + 2*qsz
 *   (pad to page boundary)
 *   used ring   : 6 + 8*qsz
 */
static inline uint64_t virtq_size_bytes(uint16_t qsz) {
    uint64_t desc_sz  = (uint64_t)qsz * 16;
    uint64_t avail_sz = 6 + 2 * (uint64_t)qsz;
    uint64_t first    = (desc_sz + avail_sz + 4095) & ~4095ULL;
    uint64_t used_sz  = 6 + 8 * (uint64_t)qsz;
    return first + used_sz;
}

/* Device memory barrier (mfence on x86_64, dmb sy on ARM64) */
static inline void virtio_mb(void) {
    arch_memory_barrier();
}

#endif /* LIMNX_VIRTIO_MMIO_H */
