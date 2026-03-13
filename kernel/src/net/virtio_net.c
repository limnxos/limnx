#define pr_fmt(fmt) "[virtio] " fmt
#include "klog.h"
#include "net/virtio_net.h"
#include "pci/pci.h"
#include "arch/interrupt.h"
#include "arch/frame.h"
#include "arch/io.h"
#include "mm/pmm.h"
#include "arch/serial.h"
#include "arch/cpu.h"

/* Forward declaration — called by net.c, defined there */
void net_rx(const void *frame, uint32_t len);

/* --- Driver state --- */

static uint16_t io_base;  /* BAR0 I/O port base */
static uint8_t  mac[6];
static uint8_t  irq_line;

/* Virtqueue 0 = RX, Virtqueue 1 = TX */
#define RX_QUEUE 0
#define TX_QUEUE 1

/* Queue size — read from device, typically 256 */
static uint16_t rxq_size;
static uint16_t txq_size;

/* RX virtqueue */
static virtq_desc_t  *rxq_desc;
static virtq_avail_t *rxq_avail;
static virtq_used_t  *rxq_used;
static uint16_t       rxq_last_used;
static uint64_t      *rxq_buffers; /* physical addresses of RX buffers */

/* TX virtqueue */
static virtq_desc_t  *txq_desc;
static virtq_avail_t *txq_avail;
static virtq_used_t  *txq_used;
static uint16_t       txq_last_used;
static uint16_t       txq_next_desc;

/* Maximum frame + virtio header */
#define RX_BUF_SIZE 1524

/* --- Helper: compute virtqueue memory layout --- */

static uint64_t virtq_size_bytes(uint16_t qsz) {
    /* Descriptors: 16 bytes each */
    uint64_t desc_size = (uint64_t)qsz * 16;
    /* Available ring: 2+2 + 2*qsz + 2 (padding) */
    uint64_t avail_size = 6 + 2 * (uint64_t)qsz;
    /* Used ring starts at page-aligned offset after desc+avail */
    uint64_t first_part = desc_size + avail_size;
    first_part = (first_part + 4095) & ~4095ULL;
    /* Used ring: 2+2 + 8*qsz + 2 */
    uint64_t used_size = 6 + 8 * (uint64_t)qsz;
    return first_part + used_size;
}

static int setup_virtqueue(uint16_t queue_idx,
                            uint16_t *out_qsz,
                            virtq_desc_t  **out_desc,
                            virtq_avail_t **out_avail,
                            virtq_used_t  **out_used) {
    /* Select queue */
    outw(io_base + VIRTIO_REG_QUEUE_SELECT, queue_idx);

    /* Read queue size */
    uint16_t qsz = inw(io_base + VIRTIO_REG_QUEUE_SIZE);
    if (qsz == 0) {
        pr_info("Queue %u size=0, skipping\n", queue_idx);
        return -1;
    }
    *out_qsz = qsz;

    /* Allocate contiguous pages for the queue */
    uint64_t total_bytes = virtq_size_bytes(qsz);
    uint32_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_contiguous(pages_needed);
    if (phys == 0) {
        pr_err("Failed to alloc %u pages for queue %u\n",
               pages_needed, queue_idx);
        return -1;
    }

    /* Zero the memory */
    uint8_t *virt = (uint8_t *)PHYS_TO_VIRT(phys);
    for (uint64_t i = 0; i < (uint64_t)pages_needed * PAGE_SIZE; i++)
        virt[i] = 0;

    /* Compute pointers */
    *out_desc  = (virtq_desc_t *)virt;
    *out_avail = (virtq_avail_t *)(virt + (uint64_t)qsz * 16);

    uint64_t avail_end = (uint64_t)qsz * 16 + 6 + 2 * (uint64_t)qsz;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    *out_used = (virtq_used_t *)(virt + used_offset);

    /* Tell device: queue address in 4096-byte units */
    outl(io_base + VIRTIO_REG_QUEUE_ADDR, (uint32_t)(phys / 4096));

    pr_info("Queue %u: size=%u pages=%u phys=%lx\n",
            queue_idx, qsz, pages_needed, phys);
    return 0;
}

/* --- RX buffer management --- */

static void rx_fill_descriptors(void) {
    for (uint16_t i = 0; i < rxq_size; i++) {
        if (rxq_buffers[i] != 0)
            continue; /* already has a buffer */

        uint64_t buf_phys = pmm_alloc_page();
        if (buf_phys == 0) break;
        rxq_buffers[i] = buf_phys;

        rxq_desc[i].addr  = buf_phys;
        rxq_desc[i].len   = RX_BUF_SIZE;
        rxq_desc[i].flags = VIRTQ_DESC_F_WRITE;
        rxq_desc[i].next  = 0;

        uint16_t avail_idx = rxq_avail->idx;
        rxq_avail->ring[avail_idx % rxq_size] = i;
        arch_memory_barrier(); /* barrier */
        rxq_avail->idx = avail_idx + 1;
    }

    /* Notify device */
    outw(io_base + VIRTIO_REG_QUEUE_NOTIFY, RX_QUEUE);
}

/* --- TX --- */

int virtio_net_tx(const void *frame, uint32_t len) {
    if (len == 0 || len > PAGE_SIZE - VIRTIO_NET_HDR_SIZE)
        return -1;

    /* Lazy-reclaim completed TX descriptors */
    while (txq_last_used != txq_used->idx) {
        uint16_t used_idx = txq_last_used % txq_size;
        uint32_t desc_id = txq_used->ring[used_idx].id;
        uint64_t buf_phys = txq_desc[desc_id].addr;
        if (buf_phys)
            pmm_free_page(buf_phys);
        txq_desc[desc_id].addr = 0;
        txq_last_used++;
    }

    /* Allocate a page for the TX buffer */
    uint64_t buf_phys = pmm_alloc_page();
    if (buf_phys == 0)
        return -1;

    uint8_t *buf = (uint8_t *)PHYS_TO_VIRT(buf_phys);

    /* Write virtio-net header (all zeros = no offload) */
    for (int i = 0; i < VIRTIO_NET_HDR_SIZE; i++)
        buf[i] = 0;

    /* Copy frame data after header */
    const uint8_t *src = (const uint8_t *)frame;
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
    arch_memory_barrier();
    txq_avail->idx = avail_idx + 1;

    /* Notify device */
    outw(io_base + VIRTIO_REG_QUEUE_NOTIFY, TX_QUEUE);
    return 0;
}

/* --- IRQ handler --- */

static void virtio_net_irq(interrupt_frame_t *frame) {
    (void)frame;

    /* Read ISR status to acknowledge interrupt */
    uint8_t isr = inb(io_base + VIRTIO_REG_ISR_STATUS);
    (void)isr;

    /* Process completed RX descriptors */
    while (rxq_last_used != rxq_used->idx) {
        uint16_t used_idx = rxq_last_used % rxq_size;
        uint32_t desc_id  = rxq_used->ring[used_idx].id;
        uint32_t total_len = rxq_used->ring[used_idx].len;

        uint8_t *buf = (uint8_t *)PHYS_TO_VIRT(rxq_buffers[desc_id]);

        /* Skip virtio-net header, pass ethernet frame to net stack */
        if (total_len > VIRTIO_NET_HDR_SIZE) {
            net_rx(buf + VIRTIO_NET_HDR_SIZE,
                   total_len - VIRTIO_NET_HDR_SIZE);
        }

        /* Mark buffer as empty so rx_fill_descriptors can reuse it */
        rxq_buffers[desc_id] = 0;
        pmm_free_page(rxq_desc[desc_id].addr);

        rxq_last_used++;
    }

    /* Replenish RX buffers */
    rx_fill_descriptors();
}

/* --- MAC access --- */

void virtio_net_get_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++)
        out[i] = mac[i];
}

/* --- Init --- */

int virtio_net_init(void) {
    pr_info("Looking for virtio-net device...\n");

    pci_device_t *dev = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_NET_DEV_ID);
    if (!dev) {
        pr_err("No virtio-net device found\n");
        return -1;
    }

    pr_info("Found at %u:%u.%u  IRQ=%u  BAR0=%x\n",
            dev->bus, dev->dev, dev->func,
            dev->irq_line, dev->bar[0]);

    /* BAR0 is I/O port (bit 0 set) */
    io_base = dev->bar[0] & ~3U;
    irq_line = dev->irq_line;

    /* Enable PCI bus mastering + I/O space */
    pci_enable_bus_mastering(dev->bus, dev->dev, dev->func);

    /* --- Virtio init sequence (legacy) --- */

    /* 1. Reset device */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, 0);

    /* 2. Acknowledge */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

    /* 3. Driver */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    /* 4. Feature negotiation — only accept MAC feature */
    uint32_t features = inl(io_base + VIRTIO_REG_DEVICE_FEATURES);
    pr_info("Device features: %x\n", features);
    outl(io_base + VIRTIO_REG_GUEST_FEATURES, features & VIRTIO_NET_F_MAC);

    /* 5. Setup virtqueues */
    if (setup_virtqueue(RX_QUEUE, &rxq_size, &rxq_desc, &rxq_avail, &rxq_used) != 0)
        goto fail;
    if (setup_virtqueue(TX_QUEUE, &txq_size, &txq_desc, &txq_avail, &txq_used) != 0)
        goto fail;

    /* Allocate RX buffer tracking array (from PMM) */
    uint32_t track_pages = ((uint64_t)rxq_size * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t track_phys = pmm_alloc_contiguous(track_pages);
    if (track_phys == 0) goto fail;
    rxq_buffers = (uint64_t *)PHYS_TO_VIRT(track_phys);
    for (uint16_t i = 0; i < rxq_size; i++)
        rxq_buffers[i] = 0;

    /* 6. Read MAC address */
    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + VIRTIO_REG_MAC + i);

    pr_info("MAC: %x:%x:%x:%x:%x:%x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 7. Set DRIVER_OK */
    outb(io_base + VIRTIO_REG_DEVICE_STATUS,
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    uint8_t status = inb(io_base + VIRTIO_REG_DEVICE_STATUS);
    pr_info("Device status: %u\n", (unsigned)status);

    /* 8. Register IRQ handler and unmask */
    arch_irq_register(irq_line, virtio_net_irq);
    arch_irq_unmask(irq_line);
    pr_info("Registered IRQ %u\n", (unsigned)irq_line);

    /* 9. Fill RX ring */
    rxq_last_used = 0;
    txq_last_used = 0;
    txq_next_desc = 0;
    rx_fill_descriptors();

    pr_info("virtio-net initialized\n");
    return 0;

fail:
    outb(io_base + VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
    pr_err("FAILED to initialize\n");
    return -1;
}
