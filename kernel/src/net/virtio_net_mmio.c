/*
 * Virtio-net driver over MMIO transport (ARM64 / QEMU virt).
 *
 * Probes virtio-mmio slots 0-31 for device_id=1 (network device).
 * Provides the same public API as virtio_net.h:
 *   virtio_net_init()
 *   virtio_net_tx(frame, len)
 *   virtio_net_get_mac(mac_out)
 *
 * RX uses an IRQ handler (GIC SPI) to process incoming packets.
 * TX uses polling for completion.
 */

#define pr_fmt(fmt) "[virtio-net-mmio] " fmt
#include "klog.h"
#include "net/virtio_net.h"
#include "net/net.h"
#include "virtio/virtio_mmio.h"
#include "arch/interrupt.h"
#include "arch/frame.h"
#include "arch/cpu.h"
#include "mm/pmm.h"
#include "mm/kheap.h"

/* ------------------------------------------------------------------ */
/* Driver state                                                       */
/* ------------------------------------------------------------------ */

static uint64_t dev_base;       /* MMIO base address */
static int      dev_slot;       /* slot index        */
static uint8_t  mac[6];

#define RX_QUEUE  0
#define TX_QUEUE  1

/* Queue sizes */
static uint16_t rxq_size;
static uint16_t txq_size;

/* RX virtqueue */
static virtq_desc_t  *rxq_desc;
static virtq_avail_t *rxq_avail;
static virtq_used_t  *rxq_used;
static uint16_t       rxq_last_used;
static uint64_t      *rxq_buffers;  /* physical addrs of RX buffers */

/* TX virtqueue */
static virtq_desc_t  *txq_desc;
static virtq_avail_t *txq_avail;
static virtq_used_t  *txq_used;
static uint16_t       txq_last_used;
static uint16_t       txq_next_desc;

/* Maximum frame + virtio header */
#define RX_BUF_SIZE  1524

/* ------------------------------------------------------------------ */
/* Virtqueue setup                                                    */
/* ------------------------------------------------------------------ */

static int setup_virtqueue(uint16_t queue_idx,
                           uint16_t *out_qsz,
                           virtq_desc_t  **out_desc,
                           virtq_avail_t **out_avail,
                           virtq_used_t  **out_used) {
    /* Select queue */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_SEL, queue_idx);

    uint16_t qsz = (uint16_t)mmio_read32(dev_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qsz == 0) {
        pr_err("queue %u max size = 0\n", queue_idx);
        return -1;
    }
    /* Cap to 256 */
    if (qsz > 256)
        qsz = 256;
    *out_qsz = qsz;

    /* Tell device our chosen queue size */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NUM, qsz);

    /* Legacy: alignment */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);

    /* Allocate contiguous pages */
    uint64_t total = virtq_size_bytes(qsz);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys  = pmm_alloc_contiguous(pages);
    if (phys == 0) {
        pr_err("failed to alloc %u pages for queue %u\n", pages, queue_idx);
        return -1;
    }

    /* Zero memory */
    uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(phys);
    for (uint64_t i = 0; i < (uint64_t)pages * PAGE_SIZE; i++)
        virt[i] = 0;

    /* Compute ring pointers */
    *out_desc  = (virtq_desc_t *)virt;
    *out_avail = (virtq_avail_t *)(virt + (uint64_t)qsz * 16);

    uint64_t avail_end   = (uint64_t)qsz * 16 + 6 + 2 * (uint64_t)qsz;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    *out_used = (virtq_used_t *)(virt + used_offset);

    /* Legacy: tell device the PFN */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_PFN,
                 (uint32_t)(phys / PAGE_SIZE));

    pr_info("queue %u: size=%u pages=%u phys=%lx\n",
            queue_idx, qsz, pages, phys);
    return 0;
}

/* ------------------------------------------------------------------ */
/* RX buffer management                                               */
/* ------------------------------------------------------------------ */

static void rx_fill_descriptors(void) {
    for (uint16_t i = 0; i < rxq_size; i++) {
        if (rxq_buffers[i] != 0)
            continue;

        uint64_t buf_phys = pmm_alloc_page();
        if (buf_phys == 0)
            break;
        rxq_buffers[i] = buf_phys;

        rxq_desc[i].addr  = buf_phys;
        rxq_desc[i].len   = RX_BUF_SIZE;
        rxq_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rxq_desc[i].next  = 0;

        uint16_t avail_idx = rxq_avail->idx;
        rxq_avail->ring[avail_idx % rxq_size] = i;
        virtio_mb();
        rxq_avail->idx = avail_idx + 1;
    }

    /* Notify device that RX buffers are available */
    virtio_mb();
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, RX_QUEUE);
}

/* ------------------------------------------------------------------ */
/* IRQ handler                                                        */
/* ------------------------------------------------------------------ */

static void virtio_net_mmio_irq(interrupt_frame_t *frame) {
    (void)frame;

    /* Read and acknowledge interrupt status */
    uint32_t isr = mmio_read32(dev_base, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (isr)
        mmio_write32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);

    /* Process completed RX descriptors */
    while (rxq_last_used != rxq_used->idx) {
        uint16_t used_idx  = rxq_last_used % rxq_size;
        uint32_t desc_id   = rxq_used->ring[used_idx].id;
        uint32_t total_len = rxq_used->ring[used_idx].len;

        if (desc_id < rxq_size && rxq_buffers[desc_id] != 0) {
            uint8_t *buf = (uint8_t *)PHYS_TO_VIRT(rxq_buffers[desc_id]);

            /* Skip virtio-net header, pass ethernet frame to net stack */
            if (total_len > VIRTIO_NET_HDR_SIZE) {
                net_rx(buf + VIRTIO_NET_HDR_SIZE,
                       total_len - VIRTIO_NET_HDR_SIZE);
            }

            /* Free buffer so rx_fill_descriptors can reallocate */
            pmm_free_page(rxq_buffers[desc_id]);
            rxq_buffers[desc_id] = 0;
        }

        rxq_last_used++;
    }

    /* Replenish RX buffers */
    rx_fill_descriptors();
}

/* ------------------------------------------------------------------ */
/* TX                                                                 */
/* ------------------------------------------------------------------ */

int virtio_net_tx(const void *frame_data, uint32_t len) {
    if (len == 0 || len > PAGE_SIZE - VIRTIO_NET_HDR_SIZE)
        return -1;

    /* Lazy-reclaim completed TX descriptors */
    while (txq_last_used != txq_used->idx) {
        uint16_t used_idx = txq_last_used % txq_size;
        uint32_t desc_id  = txq_used->ring[used_idx].id;
        if (desc_id < txq_size) {
            uint64_t buf_phys = txq_desc[desc_id].addr;
            if (buf_phys)
                pmm_free_page(buf_phys);
            txq_desc[desc_id].addr = 0;
        }
        txq_last_used++;
    }

    /* Allocate a DMA page for TX buffer */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == 0)
        return -1;

    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT(buf_phys);

    /* Write virtio-net header (all zeros = no offload) */
    for (int i = 0; i < VIRTIO_NET_HDR_SIZE; i++)
        buf[i] = 0;

    /* Copy frame data after header */
    const uint8_t *src = (const uint8_t *)frame_data;
    for (uint32_t i = 0; i < len; i++)
        buf[VIRTIO_NET_HDR_SIZE + i] = src[i];

    /* Fill descriptor */
    uint16_t desc_idx = txq_next_desc;
    txq_next_desc = (txq_next_desc + 1) % txq_size;

    txq_desc[desc_idx].addr  = buf_phys;
    txq_desc[desc_idx].len   = VIRTIO_NET_HDR_SIZE + len;
    txq_desc[desc_idx].flags = 0;
    txq_desc[desc_idx].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = txq_avail->idx;
    txq_avail->ring[avail_idx % txq_size] = desc_idx;
    virtio_mb();
    txq_avail->idx = avail_idx + 1;
    virtio_mb();

    /* Notify device */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, TX_QUEUE);

    return 0;
}

/* ------------------------------------------------------------------ */
/* MAC access                                                         */
/* ------------------------------------------------------------------ */

void virtio_net_get_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++)
        out[i] = mac[i];
}

/* ------------------------------------------------------------------ */
/* Probe and init                                                     */
/* ------------------------------------------------------------------ */

int virtio_net_init(void) {
    pr_info("probing virtio-mmio slots for network device...\n");

    /* Scan all 32 virtio-mmio slots */
    dev_base = 0;
    for (uint32_t slot = 0; slot < VIRTIO_MMIO_NUM_SLOTS; slot++) {
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

        if (devid == VIRTIO_MMIO_DEVID_NET) {
            dev_base = base;
            dev_slot = (int)slot;
            pr_info("found network device at slot %u (base=%lx)\n",
                    slot, (unsigned long)base);
            break;
        }
    }

    if (dev_base == 0) {
        pr_err("no virtio-net device found\n");
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

    /* Accept only the MAC feature */
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES,
                 features & VIRTIO_NET_F_MAC);
    virtio_mb();

    /* 5. Legacy: set guest page size */
    mmio_write32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    virtio_mb();

    /* 6. Setup virtqueues */
    if (setup_virtqueue(RX_QUEUE, &rxq_size,
                        &rxq_desc, &rxq_avail, &rxq_used) != 0)
        goto fail;
    if (setup_virtqueue(TX_QUEUE, &txq_size,
                        &txq_desc, &txq_avail, &txq_used) != 0)
        goto fail;

    /* Allocate RX buffer tracking array */
    uint32_t track_pages = ((uint64_t)rxq_size * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t track_phys  = pmm_alloc_contiguous(track_pages);
    if (track_phys == 0) {
        pr_err("failed to alloc RX buffer tracking array\n");
        goto fail;
    }
    rxq_buffers = (uint64_t *)PHYS_TO_VIRT(track_phys);
    for (uint16_t i = 0; i < rxq_size; i++)
        rxq_buffers[i] = 0;

    /* 7. Read MAC address from device config space (offset 0x100) */
    if (features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < 6; i++)
            mac[i] = mmio_read8(dev_base, VIRTIO_MMIO_CONFIG + i);
    } else {
        /* Default MAC if device doesn't provide one */
        mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
        mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
    }

    pr_info("MAC: %x:%x:%x:%x:%x:%x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 8. Set DRIVER_OK */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_DRIVER_OK);
    virtio_mb();

    uint32_t status = mmio_read32(dev_base, VIRTIO_MMIO_STATUS);
    pr_info("device status: %u\n", status);

    /* 9. Register GIC IRQ handler */
    uint8_t irq = (uint8_t)VIRTIO_MMIO_IRQ(dev_slot);
    arch_irq_register(irq, virtio_net_mmio_irq);
    arch_irq_unmask(irq);
    pr_info("registered IRQ %u (GIC SPI %u+%u)\n",
            (unsigned)irq, 48, (unsigned)dev_slot);

    /* 10. Fill RX ring and kick */
    rxq_last_used = 0;
    txq_last_used = 0;
    txq_next_desc = 0;
    rx_fill_descriptors();

    pr_info("initialized\n");
    return 0;

fail:
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
    pr_err("FAILED to initialize\n");
    return -1;
}
