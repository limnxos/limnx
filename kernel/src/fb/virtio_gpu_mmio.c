/*
 * Virtio-GPU 2D driver over MMIO transport (ARM64 / QEMU virt).
 *
 * Probes virtio-mmio slots for device_id=16 (GPU device).
 * Sets up a 2D framebuffer resource and connects it to fbcon.
 * Uses polling for command completion (no IRQ).
 */

#define pr_fmt(fmt) "[virtio-gpu-mmio] " fmt
#include "klog.h"

#include "fb/virtio_gpu_mmio.h"
#include "fb/fbcon.h"
#include "net/virtio_net.h"   /* virtq_desc_t, virtq_avail_t, virtq_used_t */
#include "virtio/virtio_mmio.h"
#include "mm/pmm.h"
#include "mm/dma.h"
#include "mm/kheap.h"
#include "arch/cpu.h"
#include "kutil.h"

/* Default framebuffer dimensions */
#define GPU_WIDTH  1024
#define GPU_HEIGHT 768
#define GPU_BPP    4     /* bytes per pixel (BGRA) */
#define GPU_RESOURCE_ID  1

static uint64_t dev_base;
static int dev_found;

/* Control queue (queue 0) */
static uint16_t       q_size;
static virtq_desc_t  *q_desc;
static virtq_avail_t *q_avail;
static virtq_used_t  *q_used;
static uint16_t       q_last_used;
static uint16_t       q_next_desc;

/* Framebuffer */
static uint8_t  *fb_virt;
static uint64_t  fb_phys;
static uint32_t  fb_width;
static uint32_t  fb_height;

/* ------------------------------------------------------------------ */
/* Virtqueue setup                                                    */
/* ------------------------------------------------------------------ */

static int setup_virtqueue(void) {
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);

    q_size = (uint16_t)mmio_read32(dev_base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (q_size == 0) {
        pr_err("queue 0 max size = 0\n");
        return -1;
    }
    if (q_size > 256)
        q_size = 256;

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NUM, q_size);
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, 4096);

    uint64_t total = virtq_size_bytes(q_size);
    uint32_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *virt = (uint8_t *)dma_alloc((uint64_t)pages * PAGE_SIZE);
    if (!virt) {
        pr_err("failed to alloc %u pages for queue\n", pages);
        return -1;
    }
    uint64_t phys = dma_virt_to_phys(virt);

    q_desc  = (virtq_desc_t *)virt;
    q_avail = (virtq_avail_t *)(virt + (uint64_t)q_size * 16);

    uint64_t avail_end   = (uint64_t)q_size * 16 + 6 + 2 * (uint64_t)q_size;
    uint64_t used_offset = (avail_end + 4095) & ~4095ULL;
    q_used = (virtq_used_t *)(virt + used_offset);

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(phys / PAGE_SIZE));

    pr_info("queue 0: size=%u pages=%u phys=%lx\n", q_size, pages, phys);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Command submission (2-descriptor: request + response)              */
/* ------------------------------------------------------------------ */

static int gpu_submit_cmd(void *req, uint32_t req_len,
                          void *resp, uint32_t resp_len) {
    uint64_t req_phys  = dma_virt_to_phys(req);
    uint64_t resp_phys = dma_virt_to_phys(resp);

    uint16_t d0 = q_next_desc;
    uint16_t d1 = (d0 + 1) % q_size;
    q_next_desc = (d0 + 2) % q_size;

    /* Descriptor 0: request (device-readable) */
    q_desc[d0].addr  = req_phys;
    q_desc[d0].len   = req_len;
    q_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d0].next  = d1;

    /* Descriptor 1: response (device-writable) */
    q_desc[d1].addr  = resp_phys;
    q_desc[d1].len   = resp_len;
    q_desc[d1].flags = VIRTQ_DESC_F_WRITE;
    q_desc[d1].next  = 0;

    /* Add to available ring */
    uint16_t avail_idx = q_avail->idx;
    q_avail->ring[avail_idx % q_size] = d0;
    virtio_mb();
    q_avail->idx = avail_idx + 1;
    virtio_mb();

    /* Notify device */
    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /* Poll for completion */
    for (int i = 0; i < 2000000; i++) {
        virtio_mb();
        if (q_used->idx != q_last_used) {
            while (q_last_used != q_used->idx)
                q_last_used++;
            uint32_t isr = mmio_read32(dev_base, VIRTIO_MMIO_INTERRUPT_STATUS);
            if (isr) mmio_write32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);
            return 0;
        }
        arch_pause();
    }

    pr_err("command timeout\n");
    return -1;
}

/* Submit a command with 3 descriptors (req + extra data + response) */
static int gpu_submit_cmd3(void *req, uint32_t req_len,
                           void *extra, uint32_t extra_len,
                           void *resp, uint32_t resp_len) {
    uint64_t req_phys   = dma_virt_to_phys(req);
    uint64_t extra_phys = dma_virt_to_phys(extra);
    uint64_t resp_phys  = dma_virt_to_phys(resp);

    uint16_t d0 = q_next_desc;
    uint16_t d1 = (d0 + 1) % q_size;
    uint16_t d2 = (d0 + 2) % q_size;
    q_next_desc = (d0 + 3) % q_size;

    q_desc[d0].addr  = req_phys;
    q_desc[d0].len   = req_len;
    q_desc[d0].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d0].next  = d1;

    q_desc[d1].addr  = extra_phys;
    q_desc[d1].len   = extra_len;
    q_desc[d1].flags = VIRTQ_DESC_F_NEXT;
    q_desc[d1].next  = d2;

    q_desc[d2].addr  = resp_phys;
    q_desc[d2].len   = resp_len;
    q_desc[d2].flags = VIRTQ_DESC_F_WRITE;
    q_desc[d2].next  = 0;

    uint16_t avail_idx = q_avail->idx;
    q_avail->ring[avail_idx % q_size] = d0;
    virtio_mb();
    q_avail->idx = avail_idx + 1;
    virtio_mb();

    mmio_write32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    for (int i = 0; i < 2000000; i++) {
        virtio_mb();
        if (q_used->idx != q_last_used) {
            while (q_last_used != q_used->idx)
                q_last_used++;
            uint32_t isr = mmio_read32(dev_base, VIRTIO_MMIO_INTERRUPT_STATUS);
            if (isr) mmio_write32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, isr);
            return 0;
        }
        arch_pause();
    }

    pr_err("command3 timeout\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* GPU 2D operations                                                  */
/* ------------------------------------------------------------------ */

static int gpu_create_resource(uint32_t res_id, uint32_t w, uint32_t h) {
    /* Allocate DMA buffer for request + response */
    uint8_t *buf = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!buf) return -1;

    virtio_gpu_resource_create_2d_t *req = (virtio_gpu_resource_create_2d_t *)buf;
    mem_zero(req, sizeof(*req));
    req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = res_id;
    req->format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width = w;
    req->height = h;

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(buf + 512);
    mem_zero(resp, sizeof(*resp));

    int ret = gpu_submit_cmd(req, sizeof(*req), resp, sizeof(*resp));
    if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        pr_err("RESOURCE_CREATE_2D failed: resp=%u\n", resp->type);
        ret = -1;
    }
    dma_free(buf, PAGE_SIZE);
    return ret;
}

static int gpu_attach_backing(uint32_t res_id, uint64_t phys_addr, uint32_t size) {
    uint8_t *buf = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!buf) return -1;

    virtio_gpu_resource_attach_backing_t *req =
        (virtio_gpu_resource_attach_backing_t *)buf;
    mem_zero(req, sizeof(*req));
    req->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id = res_id;
    req->nr_entries = 1;

    /* Memory entry follows the header in a separate descriptor */
    virtio_gpu_mem_entry_t *entry = (virtio_gpu_mem_entry_t *)(buf + 256);
    mem_zero(entry, sizeof(*entry));
    entry->addr = phys_addr;
    entry->length = size;

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(buf + 512);
    mem_zero(resp, sizeof(*resp));

    int ret = gpu_submit_cmd3(req, sizeof(*req),
                              entry, sizeof(*entry),
                              resp, sizeof(*resp));
    if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        pr_err("ATTACH_BACKING failed: resp=%u\n", resp->type);
        ret = -1;
    }
    dma_free(buf, PAGE_SIZE);
    return ret;
}

static int gpu_set_scanout(uint32_t scanout_id, uint32_t res_id,
                           uint32_t w, uint32_t h) {
    uint8_t *buf = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!buf) return -1;

    virtio_gpu_set_scanout_t *req = (virtio_gpu_set_scanout_t *)buf;
    mem_zero(req, sizeof(*req));
    req->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.x = 0;
    req->r.y = 0;
    req->r.width = w;
    req->r.height = h;
    req->scanout_id = scanout_id;
    req->resource_id = res_id;

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(buf + 512);
    mem_zero(resp, sizeof(*resp));

    int ret = gpu_submit_cmd(req, sizeof(*req), resp, sizeof(*resp));
    if (ret == 0 && resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        pr_err("SET_SCANOUT failed: resp=%u\n", resp->type);
        ret = -1;
    }
    dma_free(buf, PAGE_SIZE);
    return ret;
}

static int gpu_transfer_and_flush(uint32_t res_id, uint32_t w, uint32_t h) {
    uint8_t *buf = (uint8_t *)dma_alloc(PAGE_SIZE);
    if (!buf) return -1;

    /* TRANSFER_TO_HOST_2D */
    virtio_gpu_transfer_to_host_2d_t *xfer =
        (virtio_gpu_transfer_to_host_2d_t *)buf;
    mem_zero(xfer, sizeof(*xfer));
    xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    xfer->r.x = 0;
    xfer->r.y = 0;
    xfer->r.width = w;
    xfer->r.height = h;
    xfer->offset = 0;
    xfer->resource_id = res_id;

    virtio_gpu_ctrl_hdr_t *resp = (virtio_gpu_ctrl_hdr_t *)(buf + 512);
    mem_zero(resp, sizeof(*resp));

    int ret = gpu_submit_cmd(xfer, sizeof(*xfer), resp, sizeof(*resp));
    if (ret != 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        pr_err("TRANSFER_TO_HOST_2D failed: resp=%u\n", resp->type);
        dma_free(buf, PAGE_SIZE);
        return -1;
    }

    /* RESOURCE_FLUSH */
    virtio_gpu_resource_flush_t *flush = (virtio_gpu_resource_flush_t *)buf;
    mem_zero(flush, sizeof(*flush));
    flush->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush->r.x = 0;
    flush->r.y = 0;
    flush->r.width = w;
    flush->r.height = h;
    flush->resource_id = res_id;

    mem_zero(resp, sizeof(*resp));
    ret = gpu_submit_cmd(flush, sizeof(*flush), resp, sizeof(*resp));
    if (ret != 0 || resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        pr_err("RESOURCE_FLUSH failed: resp=%u\n", resp->type);
        ret = -1;
    }

    dma_free(buf, PAGE_SIZE);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void virtio_gpu_flush(void) {
    if (!dev_found) return;
    gpu_transfer_and_flush(GPU_RESOURCE_ID, fb_width, fb_height);
}

/* ------------------------------------------------------------------ */
/* Probe and init                                                     */
/* ------------------------------------------------------------------ */

#define VIRTIO_MMIO_DEVID_GPU 16

int virtio_gpu_mmio_init(void) {
    pr_info("probing virtio-mmio slots for GPU device...\n");

    dev_base = 0;
    dev_found = 0;
    for (uint32_t slot = 0; slot < virtio_mmio_num_devices; slot++) {
        uint64_t base = virtio_mmio_slot_base(slot);

        uint32_t magic = mmio_read32(base, VIRTIO_MMIO_MAGIC_VALUE);
        if (magic != VIRTIO_MMIO_MAGIC)
            continue;

        uint32_t devid = mmio_read32(base, VIRTIO_MMIO_DEVICE_ID);
        if (devid == VIRTIO_MMIO_DEVID_GPU) {
            dev_base = base;
            pr_info("found GPU device at slot %u (base=%lx)\n",
                    slot, (unsigned long)base);
            break;
        }
    }

    if (dev_base == 0) {
        pr_info("no virtio-gpu device found (framebuffer disabled)\n");
        return -1;
    }

    /* --- Virtio init sequence (legacy MMIO) --- */

    /* 1. Reset */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, 0);
    virtio_mb();

    /* 2. Acknowledge */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACK);
    virtio_mb();

    /* 3. Driver */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    virtio_mb();

    /* 4. Feature negotiation — accept no special features */
    mmio_write32(dev_base, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    mmio_write32(dev_base, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    virtio_mb();

    /* 5. Legacy: set guest page size */
    mmio_write32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
    virtio_mb();

    /* 6. Setup controlq (queue 0) */
    if (setup_virtqueue() != 0)
        goto fail;

    /* 7. Set DRIVER_OK */
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                 VIRTIO_STATUS_DRIVER_OK);
    virtio_mb();

    q_last_used = 0;
    q_next_desc = 0;

    /* --- Setup 2D framebuffer --- */

    fb_width  = GPU_WIDTH;
    fb_height = GPU_HEIGHT;
    uint32_t fb_size = fb_width * fb_height * GPU_BPP;
    uint32_t fb_pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;

    fb_virt = (uint8_t *)dma_alloc((uint64_t)fb_pages * PAGE_SIZE);
    if (!fb_virt) {
        pr_err("failed to alloc %u pages for framebuffer\n", fb_pages);
        goto fail;
    }
    fb_phys = dma_virt_to_phys(fb_virt);
    mem_zero(fb_virt, (uint64_t)fb_pages * PAGE_SIZE);

    /* Create GPU resource */
    if (gpu_create_resource(GPU_RESOURCE_ID, fb_width, fb_height) != 0)
        goto fail;

    /* Attach framebuffer memory as backing storage */
    if (gpu_attach_backing(GPU_RESOURCE_ID, fb_phys, fb_size) != 0)
        goto fail;

    /* Set scanout 0 to our resource */
    if (gpu_set_scanout(0, GPU_RESOURCE_ID, fb_width, fb_height) != 0)
        goto fail;

    /* Initial flush */
    gpu_transfer_and_flush(GPU_RESOURCE_ID, fb_width, fb_height);

    dev_found = 1;

    /* Connect to fbcon */
    fbcon_init(fb_virt, fb_width, fb_height, fb_width * GPU_BPP, GPU_BPP * 8);

    pr_info("initialized %ux%u framebuffer (phys=%lx)\n",
            fb_width, fb_height, (unsigned long)fb_phys);
    return 0;

fail:
    mmio_write32(dev_base, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_FAILED);
    pr_err("FAILED to initialize\n");
    return -1;
}
