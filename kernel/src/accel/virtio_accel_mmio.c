/*
 * Virtio-accel driver over MMIO transport.
 *
 * Probes virtio-mmio slots for device_id=48 (compute accelerator).
 * Provides synchronous polled tensor operations:
 *   matmul, softmax, rmsnorm, rope, silu, dequant, etc.
 *
 * Tensor data is NOT copied through the virtqueue — the request
 * contains physical addresses of input/output buffers, and the
 * QEMU backend reads/writes guest RAM directly.
 */

#define pr_fmt(fmt) "[virtio-accel] " fmt
#include "klog.h"
#include "accel/virtio_accel_mmio.h"
#include "virtio/virtio_mmio.h"
#include "net/virtio_net.h"   /* virtq_desc_t, virtq_avail_t, virtq_used_t */
#include "mm/dma.h"
#include "mm/pmm.h"
#include "arch/cpu.h"
#include "limnx/virtio_accel.h"

/*
 * On x86_64, virtio-mmio is not available (PCI transport is used).
 * The MMIO slot scan is ARM64-only. On x86_64, init returns -1 immediately.
 * Future: add PCI virtio-accel detection for x86_64.
 */

/* Driver state */
static uint64_t dev_base;
static int      dev_found;

static uint16_t       q_size;
static virtq_desc_t  *q_desc;
static virtq_avail_t *q_avail;
static virtq_used_t  *q_used;
static uint16_t       q_last_used;
static uint16_t       q_next_desc;

/* Device config */
static uint32_t dev_features;
static uint32_t dev_max_bytes;
static uint32_t dev_num_cu;

/* Pre-allocated DMA buffers for request/response */
static virtio_accel_req_t  *req_buf;
static virtio_accel_resp_t *resp_buf;
static uint64_t req_phys;
static uint64_t resp_phys;

static int setup_virtqueue(void) {
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);

    q_size = (uint16_t)mmio_read32(dev_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (q_size == 0) {
        pr_err("queue 0 max size = 0\n");
        return -1;
    }
    if (q_size > 64)
        q_size = 64;

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NUM, q_size);
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);

    uint64_t total = virtq_size_bytes(q_size);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *virt = (uint8_t *)dma_alloc((uint64_t)pages * PAGE_SIZE);
    if (!virt) {
        pr_err("failed to alloc queue pages\n");
        return -1;
    }
    uint64_t phys = dma_virt_to_phys(virt);

    q_desc  = (virtq_desc_t *)virt;
    q_avail = (virtq_avail_t *)(virt + (uint64_t)q_size * 16);

    uint64_t avail_end   = (uint64_t)q_size * 16 + 6 + 2 * (uint64_t)q_size;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    q_used = (virtq_used_t *)(virt + used_offset);

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));

    pr_info("queue 0: size=%u phys=%lx\n", q_size, phys);
    return 0;
}

int virtio_accel_init(void) {
    dev_found = 0;

#if defined(__x86_64__)
    /* x86_64 uses PCI, not MMIO. No virtio-accel via PCI yet. */
    pr_info("No accelerator device found (x86_64 PCI support planned)\n");
    return -1;
#endif

    /* Scan MMIO slots for device ID 48 (ARM64 virtio-mmio) */
    for (uint32_t slot = 0; slot < virtio_mmio_num_devices; slot++) {
        uint64_t base = virtio_mmio_slot_base(slot);
        uint32_t magic = mmio_read32(base, VIRTIO_MMIO_MAGIC_VALUE);
        if (magic != VIRTIO_MMIO_MAGIC) continue;

        uint32_t devid = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);
        if (devid != VIRTIO_ACCEL_DEVICE_ID) continue;

        dev_base = base;
        pr_info("Found accelerator at slot %u (base %lx)\n", slot, base);

        /* Reset */
        mmio_write32(dev_base, VIRTIO_MMIO_STATUS, 0);

        /* ACK + DRIVER */
        uint32_t status = VIRTIO_STATUS_ACK;
        mmio_write32(dev_base, VIRTIO_MMIO_STATUS, status);
        status |= VIRTIO_STATUS_DRIVER;
        mmio_write32(dev_base, VIRTIO_MMIO_STATUS, status);

        /* Read features */
        dev_features = mmio_read32(dev_base, VIRTIO_MMIO_DEVICE_FEATURES);

        /* Legacy: set guest page size */
        mmio_write32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);

        /* Accept all features */
        mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES, dev_features);

        /* Setup virtqueue */
        if (setup_virtqueue() != 0) goto fail;

        /* DRIVER_OK */
        status |= VIRTIO_STATUS_DRIVER_OK;
        mmio_write32(dev_base, VIRTIO_MMIO_STATUS, status);

        /* Read device config */
        dev_max_bytes = mmio_read32(dev_base, VIRTIO_MMIO_CONFIG + 0);
        dev_num_cu    = mmio_read32(dev_base, VIRTIO_MMIO_CONFIG + 4);

        /* Allocate DMA buffers for req/resp */
        uint8_t *dma_page = (uint8_t *)dma_alloc(PAGE_SIZE);
        if (!dma_page) goto fail;
        req_buf   = (virtio_accel_req_t *)dma_page;
        resp_buf  = (virtio_accel_resp_t *)(dma_page + 512);
        req_phys  = dma_virt_to_phys(dma_page);
        resp_phys = req_phys + 512;

        dev_found = 1;
        pr_info("Initialized: features=0x%x max_bytes=%u CUs=%u\n",
                dev_features, dev_max_bytes, dev_num_cu);
        return 0;

    fail:
        mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
        pr_err("FAILED to initialize\n");
        return -1;
    }

    /* No device found — not an error, just no acceleration */
    pr_info("No accelerator device found (CPU-only mode)\n");
    return -1;
}

int virtio_accel_available(void) {
    return dev_found;
}

void virtio_accel_get_info(uint32_t *features, uint32_t *max_bytes, uint32_t *n_cu) {
    if (features)  *features  = dev_found ? dev_features : 0;
    if (max_bytes) *max_bytes = dev_found ? dev_max_bytes : 0;
    if (n_cu)      *n_cu      = dev_found ? dev_num_cu : 0;
}

int virtio_accel_submit(uint32_t op, uint64_t a_phys, uint32_t a_rows, uint32_t a_cols,
                        uint64_t b_phys, uint32_t b_rows, uint32_t b_cols,
                        uint64_t out_phys, uint32_t out_rows, uint32_t out_cols,
                        uint32_t param0, uint32_t param1, uint32_t fparam0_bits) {
    if (!dev_found) return -1;

    static uint32_t next_id = 1;

    /* Fill request */
    req_buf->op       = op;
    req_buf->id       = next_id++;
    req_buf->flags    = 0;
    req_buf->dtype    = VACCEL_DTYPE_F32;
    req_buf->a_addr   = a_phys;
    req_buf->a_rows   = a_rows;
    req_buf->a_cols   = a_cols;
    req_buf->b_addr   = b_phys;
    req_buf->b_rows   = b_rows;
    req_buf->b_cols   = b_cols;
    req_buf->out_addr = out_phys;
    req_buf->out_rows = out_rows;
    req_buf->out_cols = out_cols;
    req_buf->param0   = param0;
    req_buf->param1   = param1;
    /* Bitcast uint32_t → float without using FP registers */
    {
        uint8_t *dst = (uint8_t *)&req_buf->fparam0;
        uint8_t *src = (uint8_t *)&fparam0_bits;
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    }

    /* Clear response */
    resp_buf->id     = 0;
    resp_buf->status = 0xFF;
    resp_buf->cycles = 0;

    /* Build 2-descriptor chain: req (readable) → resp (writable) */
    uint16_t d0 = q_next_desc;
    uint16_t d1 = (d0 + 1) % q_size;
    q_next_desc = (d1 + 1) % q_size;

    q_desc[d0].addr  = req_phys;
    q_desc[d0].len   = sizeof(virtio_accel_req_t);
    q_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d0].next  = d1;

    q_desc[d1].addr  = resp_phys;
    q_desc[d1].len   = sizeof(virtio_accel_resp_t);
    q_desc[d1].flags = VIRTQ_DESC_F_WRITE;
    q_desc[d1].next  = 0;

    /* Post to available ring */
    uint16_t avail_idx = q_avail->idx;
    q_avail->ring[avail_idx % q_size] = d0;
    virtio_mb();
    q_avail->idx = avail_idx + 1;

    /* Notify device */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion */
    for (int i = 0; i < 10000000; i++) {
        virtio_mb();
        if (q_used->idx != q_last_used) {
            q_last_used = q_used->idx;
            if (resp_buf->status == VACCEL_STATUS_OK)
                return 0;
            pr_warn("op %u failed: status=%u\n", op, resp_buf->status);
            return -(int)resp_buf->status;
        }
        arch_pause();
    }

    pr_err("op %u timed out\n", op);
    return -1;
}
