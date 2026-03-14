/*
 * Virtio-blk driver over MMIO transport (ARM64 / QEMU virt).
 *
 * Probes virtio-mmio slots 0-31 for device_id=2 (block device).
 * Provides the same public API as virtio_blk.h:
 *   virtio_blk_init()
 *   virtio_blk_read(sector, buf)
 *   virtio_blk_write(sector, buf)
 *
 * Uses polling for completion (no IRQ).
 */

#define pr_fmt(fmt) "[virtio-blk-mmio] " fmt
#include "klog.h"
#include "blk/virtio_blk.h"
#include "net/virtio_net.h"   /* virtq_desc_t, virtq_avail_t, virtq_used_t */
#include "virtio/virtio_mmio.h"
#include "dtb/dtb.h"

/* Runtime virtio-mmio parameters (defaults: QEMU virt) */
uint64_t virtio_mmio_base_addr = 0x0A000000ULL;
uint32_t virtio_mmio_num_devices = 32;
#include "mm/pmm.h"
#include "mm/dma.h"
#include "mm/kheap.h"
#include "arch/cpu.h"

/* ------------------------------------------------------------------ */
/* Driver state                                                       */
/* ------------------------------------------------------------------ */

static uint64_t dev_base;       /* MMIO base address of the found device */
static int      dev_slot;       /* slot index (for debug / IRQ)          */

/* Single request queue (queue 0) */
static uint16_t       q_size;
static virtq_desc_t  *q_desc;
static virtq_avail_t *q_avail;
static virtq_used_t  *q_used;
static uint16_t       q_last_used;
static uint16_t       q_next_desc;

/* ------------------------------------------------------------------ */
/* Virtqueue setup                                                    */
/* ------------------------------------------------------------------ */

static int setup_virtqueue(void) {
    /* Select queue 0 */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);

    q_size = (uint16_t)mmio_read32(dev_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (q_size == 0) {
        pr_err("queue 0 max size = 0\n");
        return -1;
    }
    /* Cap to 256 to keep memory usage reasonable */
    if (q_size > 256)
        q_size = 256;

    /* Tell the device how many entries we use */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NUM, q_size);

    /* Legacy: set alignment to 4096 */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);

    /* Allocate contiguous pages for descriptor + avail + used */
    uint64_t total = virtq_size_bytes(q_size);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *virt = (uint8_t *)dma_alloc((uint64_t)pages * PAGE_SIZE);
    if (!virt) {
        pr_err("failed to alloc %u pages for queue\n", pages);
        return -1;
    }
    uint64_t phys = dma_virt_to_phys(virt);

    /* Compute ring pointers */
    q_desc  = (virtq_desc_t *)virt;
    q_avail = (virtq_avail_t *)(virt + (uint64_t)q_size * 16);

    uint64_t avail_end   = (uint64_t)q_size * 16 + 6 + 2 * (uint64_t)q_size;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    q_used = (virtq_used_t *)(virt + used_offset);

    /* Legacy: tell device the PFN of the queue */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));

    pr_info("queue 0: size=%u pages=%u phys=%lx\n", q_size, pages, phys);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Synchronous I/O (3-descriptor chain, polled completion)            */
/* ------------------------------------------------------------------ */

static int blk_do_request(uint32_t type, uint64_t sector, void *data_buf) {
    /* Allocate a page for request header + status byte */
    uint8_t *req_virt = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!req_virt) return -1;
    uint64_t req_phys = dma_virt_to_phys(req_virt);

    /* Fill request header at offset 0 */
    virtio_blk_req_t *hdr = (virtio_blk_req_t *)req_virt;
    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;

    /* Allocate a separate DMA page for the data buffer */
    uint8_t *data_virt = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!data_virt) {
        dma_free(req_virt, PAGE_SIZE);
        return -1;
    }
    uint64_t data_phys = dma_virt_to_phys(data_virt);

    /* If write, copy data into DMA buffer */
    if (type == VIRTIO_BLK_T_OUT) {
        const uint8_t *src = (const uint8_t *)data_buf;
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
            data_virt[i] = src[i];
    }

    /* Status byte at offset 512 in the request page */
    uint64_t status_phys = req_phys + 512;
    uint8_t *status_virt = req_virt + 512;
    *status_virt = 0xFF;  /* sentinel */

    /* Build 3-descriptor chain */
    uint16_t d0 = q_next_desc;
    uint16_t d1 = (d0 + 1) % q_size;
    uint16_t d2 = (d0 + 2) % q_size;
    q_next_desc = (d0 + 3) % q_size;

    /* Descriptor 0: header (device-readable) */
    q_desc[d0].addr  = req_phys;
    q_desc[d0].len   = sizeof(virtio_blk_req_t);
    q_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d0].next  = d1;

    /* Descriptor 1: data buffer */
    q_desc[d1].addr  = data_phys;
    q_desc[d1].len   = VIRTIO_BLK_SECTOR_SIZE;
    q_desc[d1].flags = VIRTQ_DESC_F_NEXT |
                       (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    q_desc[d1].next  = d2;

    /* Descriptor 2: status (device-writable) */
    q_desc[d2].addr  = status_phys;
    q_desc[d2].len   = 1;
    q_desc[d2].flags = VIRTQ_DESC_F_WRITE;
    q_desc[d2].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = q_avail->idx;
    q_avail->ring[avail_idx % q_size] = d0;
    virtio_mb();
    q_avail->idx = avail_idx + 1;
    virtio_mb();

    /* Notify device (queue 0) */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion with timeout */
    int completed = 0;
    for (int i = 0; i < 1000000; i++) {
        virtio_mb();
        if (q_used->idx != q_last_used) {
            completed = 1;
            break;
        }
        arch_pause();
    }

    if (!completed) {
        pr_err("timeout waiting for blk request (sector=%lu type=%u)\n",
               sector, type);
        dma_free(req_virt, PAGE_SIZE);
        dma_free(data_virt, PAGE_SIZE);
        return -1;
    }

    /* Advance last_used to consume the used entry */
    while (q_last_used != q_used->idx)
        q_last_used++;

    /* Acknowledge any pending interrupt (required to clear interrupt status) */
    uint32_t isr = mmio_read32(dev_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr)
        mmio_write32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    /* Check status byte */
    virtio_mb();
    int result = (*status_virt == VIRTIO_BLK_S_OK) ? 0 : -1;

    /* If read, copy data out */
    if (type == VIRTIO_BLK_T_IN && result == 0) {
        uint8_t *dst = (uint8_t *)data_buf;
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
            dst[i] = data_virt[i];
    }

    if (result != 0) {
        pr_err("request failed: status=%u (sector=%lu type=%u)\n",
               (unsigned)*status_virt, sector, type);
    }

    /* Free DMA buffers */
    pmm_free_page(req_phys);
    pmm_free_page(data_phys);

    return result;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int virtio_blk_read(uint64_t sector, void *buf) {
    return blk_do_request(VIRTIO_BLK_T_IN, sector, buf);
}

int virtio_blk_write(uint64_t sector, const void *buf) {
    return blk_do_request(VIRTIO_BLK_T_OUT, sector, (void *)buf);
}

/* ------------------------------------------------------------------ */
/* Probe and init                                                     */
/* ------------------------------------------------------------------ */

int virtio_blk_init(void) {
    pr_info("probing virtio-mmio slots for block device...\n");

    /* Scan all 32 virtio-mmio slots */
    dev_base = 0;
    for (uint32_t slot = 0; slot < virtio_mmio_num_devices; slot++) {
        uint64_t base = virtio_mmio_slot_base(slot);

        uint32_t magic = mmio_read32(base, VIRTIO_MMIO_MAGIC_VALUE);
        if (magic != VIRTIO_MMIO_MAGIC)
            continue;

        uint32_t version  = mmio_read32(base, VIRTIO_MMIO_VERSION);
        uint32_t devid    = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);
        uint32_t vendorid = mmio_read32(base, VIRTIO_MMIO_VENDOR_ID);

        pr_info("slot %u: magic=%lx ver=%u devid=%u vendor=%lx\n",
                slot, (unsigned long)magic, version, devid,
                (unsigned long)vendorid);

        if (devid == VIRTIO_MMIO_DEVID_BLK) {
            dev_base = base;
            dev_slot = (int)slot;
            pr_info("found block device at slot %u (base=%lx)\n",
                    slot, (unsigned long)base);
            break;
        }
    }

    if (dev_base == 0) {
        pr_err("no virtio-blk device found\n");
        return -1;
    }

    /* --- Virtio init sequence (legacy MMIO) --- */

    /* 1. Reset device */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, 0);
    virtio_mb();

    /* 2. Acknowledge */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    virtio_mb();

    /* 3. Driver */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    virtio_mb();

    /* 4. Feature negotiation */
    mmio_write32(dev_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    uint32_t features = mmio_read32(dev_base, VIRTIO_MMIO_DEVICE_FEATURES);
    pr_info("device features: %lx\n", (unsigned long)features);

    /* Accept no special features */
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    virtio_mb();

    /* 5. Legacy: set guest page size */
    mmio_write32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    virtio_mb();

    /* 6. Setup request queue (queue 0) */
    if (setup_virtqueue() != 0)
        goto fail;

    /* 7. Set DRIVER_OK */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_DRIVER_OK);
    virtio_mb();

    uint32_t status = mmio_read32(dev_base, VIRTIO_MMIO_STATUS);
    pr_info("device status: %u\n", status);

    q_last_used = 0;
    q_next_desc = 0;

    pr_info("initialized (polling mode)\n");
    return 0;

fail:
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
    pr_err("FAILED to initialize\n");
    return -1;
}
