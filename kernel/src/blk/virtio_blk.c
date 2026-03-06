#include "blk/virtio_blk.h"
#include "net/virtio_net.h"   /* for VIRTIO_VENDOR_ID and virtqueue structs */
#include "pci/pci.h"
#include "idt/idt.h"
#include "io.h"
#include "mm/pmm.h"
#include "sched/sched.h"
#include "serial.h"

/* --- Driver state --- */

static uint16_t io_base;
static uint8_t  irq_line;

/* Single request queue (queue 0) */
static uint16_t q_size;
static virtq_desc_t  *q_desc;
static virtq_avail_t *q_avail;
static virtq_used_t  *q_used;
static uint16_t       q_last_used;
static uint16_t       q_next_desc;

/* Completion flag set by IRQ handler */
static volatile int blk_irq_fired;

/* --- Helper: compute virtqueue memory layout (same as virtio_net) --- */

static uint64_t virtq_size_bytes(uint16_t qsz) {
    uint64_t desc_size = (uint64_t)qsz * 16;
    uint64_t avail_size = 6 + 2 * (uint64_t)qsz;
    uint64_t first_part = desc_size + avail_size;
    first_part = (first_part + 4095) & ~4095ULL;
    uint64_t used_size = 6 + 8 * (uint64_t)qsz;
    return first_part + used_size;
}

static int setup_virtqueue(void) {
    /* Select queue 0 */
    outw(io_base + VIRTIO_REG_QUEUE_SELECT, 0);

    q_size = inw(io_base + VIRTIO_REG_QUEUE_SIZE);
    if (q_size == 0) {
        serial_puts("[vblk] Queue 0 size=0\n");
        return -1;
    }

    uint64_t total_bytes = virtq_size_bytes(q_size);
    uint32_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages_needed);
    if (phys == 0) {
        serial_puts("[vblk] Failed to alloc queue pages\n");
        return -1;
    }

    uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(phys);
    for (uint64_t i = 0; i < (uint64_t)pages_needed * PAGE_SIZE; i++)
        virt[i] = 0;

    q_desc  = (virtq_desc_t *)virt;
    q_avail = (virtq_avail_t *)(virt + (uint64_t)q_size * 16);

    uint64_t avail_end = (uint64_t)q_size * 16 + 6 + 2 * (uint64_t)q_size;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    q_used = (virtq_used_t *)(virt + used_offset);

    outl(io_base + VIRTIO_REG_QUEUE_ADDR, (uint32_t)(phys / 4096));

    serial_printf("[vblk] Queue 0: size=%u pages=%u phys=%lx\n",
                  q_size, pages_needed, phys);
    return 0;
}

/* --- IRQ handler --- */

static void virtio_blk_irq(interrupt_frame_t *frame) {
    (void)frame;

    /* Read ISR status to acknowledge interrupt */
    uint8_t isr = inb(io_base + VIRTIO_REG_ISR_STATUS);
    (void)isr;

    blk_irq_fired = 1;
}

/* --- Synchronous I/O (3-descriptor chain) --- */

static int blk_do_request(uint32_t type, uint64_t sector, void *data_buf) {
    /* Allocate a page for the request header + status byte */
    uint64_t req_phys = pmm_alloc_page();
    if (req_phys == 0) return -1;

    uint8_t *req_virt = (uint8_t *)PHYS_TO_VIRT(req_phys);

    /* Zero the page */
    for (int i = 0; i < (int)PAGE_SIZE; i++)
        req_virt[i] = 0;

    /* Header at offset 0 */
    virtio_blk_req_t *hdr = (virtio_blk_req_t *)req_virt;
    hdr->type     = type;
    hdr->reserved = 0;
    hdr->sector   = sector;

    /* Data buffer: use a separate PMM page for DMA */
    uint64_t data_phys = pmm_alloc_page();
    if (data_phys == 0) {
        pmm_free_page(req_phys);
        return -1;
    }
    uint8_t *data_virt = (uint8_t *)PHYS_TO_VIRT(data_phys);

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

    /* Descriptor 0: header (readable) */
    q_desc[d0].addr  = req_phys;
    q_desc[d0].len   = sizeof(virtio_blk_req_t);
    q_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d0].next  = d1;

    /* Descriptor 1: data */
    q_desc[d1].addr  = data_phys;
    q_desc[d1].len   = VIRTIO_BLK_SECTOR_SIZE;
    q_desc[d1].flags = VIRTQ_DESC_F_NEXT |
                        (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    q_desc[d1].next  = d2;

    /* Descriptor 2: status (writable) */
    q_desc[d2].addr  = status_phys;
    q_desc[d2].len   = 1;
    q_desc[d2].flags = VIRTQ_DESC_F_WRITE;
    q_desc[d2].next  = 0;

    /* Reset completion flag */
    blk_irq_fired = 0;

    /* Add to available ring */
    uint16_t avail_idx = q_avail->idx;
    q_avail->ring[avail_idx % q_size] = d0;
    __asm__ volatile ("" ::: "memory");
    q_avail->idx = avail_idx + 1;

    /* Notify device */
    outw(io_base + VIRTIO_REG_QUEUE_NOTIFY, 0);

    /* Busy-wait for completion */
    for (int i = 0; i < 100000; i++) {
        if (blk_irq_fired)
            break;
        sched_yield();
    }

    /* Process used ring */
    while (q_last_used != q_used->idx)
        q_last_used++;

    /* Check status */
    int result = (*status_virt == VIRTIO_BLK_S_OK) ? 0 : -1;

    /* If read, copy data out */
    if (type == VIRTIO_BLK_T_IN && result == 0) {
        uint8_t *dst = (uint8_t *)data_buf;
        for (int i = 0; i < VIRTIO_BLK_SECTOR_SIZE; i++)
            dst[i] = data_virt[i];
    }

    /* Free DMA buffers */
    pmm_free_page(req_phys);
    pmm_free_page(data_phys);

    return result;
}

int virtio_blk_read(uint64_t sector, void *buf) {
    return blk_do_request(VIRTIO_BLK_T_IN, sector, buf);
}

int virtio_blk_write(uint64_t sector, const void *buf) {
    return blk_do_request(VIRTIO_BLK_T_OUT, sector, (void *)buf);
}

/* --- Init --- */

int virtio_blk_init(void) {
    serial_puts("[vblk] Looking for virtio-blk device...\n");

    pci_device_t *dev = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_BLK_DEV_ID);
    if (!dev) {
        serial_puts("[vblk] No virtio-blk device found\n");
        return -1;
    }

    serial_printf("[vblk] Found at %u:%u.%u  IRQ=%u  BAR0=%x\n",
                  dev->bus, dev->dev, dev->func,
                  dev->irq_line, dev->bar[0]);

    io_base = dev->bar[0] & ~3U;
    irq_line = dev->irq_line;

    pci_enable_bus_mastering(dev->bus, dev->dev, dev->func);

    /* --- Virtio init sequence (legacy) --- */

    /* 1. Reset */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);

    /* 2. Acknowledge */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

    /* 3. Driver */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 4. Feature negotiation — accept no special features */
    uint32_t features = inl(io_base + VIRTIO_REG_DEVICE_FEATURES);
    serial_printf("[vblk] Device features: %x\n", features);
    outl(io_base + VIRTIO_REG_GUEST_FEATURES, 0);

    /* 5. Setup request queue (queue 0) */
    if (setup_virtqueue() != 0)
        goto fail;

    /* 6. Set DRIVER_OK */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    uint8_t status = inb(io_base + VIRTIO_REG_DEVICE_STATUS);
    serial_printf("[vblk] Device status: %u\n", (unsigned)status);

    /* 7. Register IRQ handler and unmask */
    irq_register_handler(irq_line, virtio_blk_irq);
    irq_unmask(irq_line);
    serial_printf("[vblk] Registered IRQ %u\n", (unsigned)irq_line);

    q_last_used = 0;
    q_next_desc = 0;
    blk_irq_fired = 0;

    serial_puts("[vblk] virtio-blk initialized\n");
    return 0;

fail:
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
    serial_puts("[vblk] FAILED to initialize\n");
    return -1;
}
