/*
 * virtio-accel QEMU device — tensor compute accelerator
 *
 * Custom virtio-mmio device (ID 48) that accelerates tensor operations
 * for the Limnx guest kernel. Reads tensor data from guest RAM, performs
 * compute on the host, writes results back.
 *
 * Supported operations: matmul, softmax, rmsnorm, rope, silu, elemul,
 * elemadd, dequant, ping.
 *
 * Build: copy into QEMU source tree and add to hw/virtio/meson.build.
 * See README.md for instructions.
 *
 * QEMU command line:
 *   -device virtio-accel-device
 */

#include "qemu/osdep.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-mmio.h"
#include "qemu/log.h"
#include "sysemu/dma.h"
#include <math.h>

#include "virtio-accel.h"

/* ------------------------------------------------------------------ */
/* Tensor compute on host CPU (could be replaced with CUDA/OpenCL)    */
/* ------------------------------------------------------------------ */

static void host_matmul(float *out, const float *a, const float *b,
                        uint32_t a_rows, uint32_t a_cols,
                        uint32_t b_rows, uint32_t b_cols) {
    /* out[a_rows x b_cols] = a[a_rows x a_cols] @ b[b_rows x b_cols] */
    (void)b_rows;
    uint32_t out_cols = b_cols;
    for (uint32_t i = 0; i < a_rows; i++) {
        for (uint32_t j = 0; j < out_cols; j++) {
            float sum = 0.0f;
            for (uint32_t k = 0; k < a_cols; k++) {
                sum += a[i * a_cols + k] * b[k * b_cols + j];
            }
            out[i * out_cols + j] = sum;
        }
    }
}

static void host_softmax(float *x, uint32_t size) {
    float max_val = x[0];
    for (uint32_t i = 1; i < size; i++) {
        if (x[i] > max_val) max_val = x[i];
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i < size; i++) {
        x[i] = expf(x[i] - max_val);
        sum += x[i];
    }
    for (uint32_t i = 0; i < size; i++) {
        x[i] /= sum;
    }
}

static void host_rmsnorm(float *out, const float *x, const float *weight,
                         uint32_t dim, float eps) {
    float ss = 0.0f;
    for (uint32_t i = 0; i < dim; i++) {
        ss += x[i] * x[i];
    }
    ss = 1.0f / sqrtf(ss / (float)dim + eps);
    for (uint32_t i = 0; i < dim; i++) {
        out[i] = x[i] * ss * weight[i];
    }
}

static void host_rope(float *vec, uint32_t dim, uint32_t pos, float theta) {
    for (uint32_t i = 0; i < dim; i += 2) {
        float freq = 1.0f / powf(theta, (float)i / (float)dim);
        float angle = (float)pos * freq;
        float cos_a = cosf(angle), sin_a = sinf(angle);
        float v0 = vec[i], v1 = vec[i + 1];
        vec[i]     = v0 * cos_a - v1 * sin_a;
        vec[i + 1] = v0 * sin_a + v1 * cos_a;
    }
}

static void host_silu(float *x, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        x[i] = x[i] / (1.0f + expf(-x[i]));
    }
}

static void host_elemul(float *out, const float *a, const float *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[i] = a[i] * b[i];
    }
}

static void host_elemadd(float *out, const float *a, const float *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        out[i] = a[i] + b[i];
    }
}

/* ------------------------------------------------------------------ */
/* Virtqueue handler — called when guest notifies                     */
/* ------------------------------------------------------------------ */

static void virtio_accel_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOAccel *s = VIRTIO_ACCEL(vdev);
    VirtQueueElement *elem;

    while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
        if (elem->out_num < 1 || elem->in_num < 1) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "virtio-accel: bad descriptor chain\n");
            virtqueue_push(vq, elem, 0);
            g_free(elem);
            continue;
        }

        /* Read request from guest */
        VirtIOAccelReq req;
        size_t req_sz = iov_to_buf(elem->out_sg, elem->out_num, 0,
                                   &req, sizeof(req));
        if (req_sz < sizeof(req)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "virtio-accel: short request (%zu)\n", req_sz);
            VirtIOAccelResp resp = { .id = 0, .status = VACCEL_STATUS_ERROR };
            iov_from_buf(elem->in_sg, elem->in_num, 0, &resp, sizeof(resp));
            virtqueue_push(vq, elem, sizeof(resp));
            g_free(elem);
            continue;
        }

        VirtIOAccelResp resp;
        resp.id = req.id;
        resp.status = VACCEL_STATUS_OK;
        resp.cycles = 0;

        AddressSpace *as = &address_space_memory;

        switch (req.op) {
        case VACCEL_OP_MATMUL: {
            uint32_t a_n = req.a_rows * req.a_cols;
            uint32_t b_n = req.b_rows * req.b_cols;
            uint32_t o_n = req.a_rows * req.b_cols;

            float *a_buf = g_malloc(a_n * sizeof(float));
            float *b_buf = g_malloc(b_n * sizeof(float));
            float *o_buf = g_malloc(o_n * sizeof(float));

            dma_memory_read(as, req.a_addr, a_buf, a_n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_read(as, req.b_addr, b_buf, b_n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);

            host_matmul(o_buf, a_buf, b_buf,
                        req.a_rows, req.a_cols, req.b_rows, req.b_cols);

            dma_memory_write(as, req.out_addr, o_buf, o_n * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);

            g_free(a_buf);
            g_free(b_buf);
            g_free(o_buf);
            break;
        }

        case VACCEL_OP_SOFTMAX: {
            uint32_t n = req.a_cols;
            float *buf = g_malloc(n * sizeof(float));
            dma_memory_read(as, req.a_addr, buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_softmax(buf, n);
            dma_memory_write(as, req.out_addr, buf, n * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(buf);
            break;
        }

        case VACCEL_OP_RMSNORM: {
            uint32_t dim = req.a_cols;
            float *x_buf = g_malloc(dim * sizeof(float));
            float *w_buf = g_malloc(dim * sizeof(float));
            float *o_buf = g_malloc(dim * sizeof(float));
            dma_memory_read(as, req.a_addr, x_buf, dim * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_read(as, req.b_addr, w_buf, dim * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_rmsnorm(o_buf, x_buf, w_buf, dim, req.fparam0);
            dma_memory_write(as, req.out_addr, o_buf, dim * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(x_buf);
            g_free(w_buf);
            g_free(o_buf);
            break;
        }

        case VACCEL_OP_ROPE: {
            uint32_t dim = req.a_cols;
            float *buf = g_malloc(dim * sizeof(float));
            dma_memory_read(as, req.a_addr, buf, dim * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_rope(buf, dim, req.param0, req.fparam0);
            dma_memory_write(as, req.out_addr, buf, dim * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(buf);
            break;
        }

        case VACCEL_OP_SILU: {
            uint32_t n = req.a_cols;
            float *buf = g_malloc(n * sizeof(float));
            dma_memory_read(as, req.a_addr, buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_silu(buf, n);
            dma_memory_write(as, req.out_addr, buf, n * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(buf);
            break;
        }

        case VACCEL_OP_ELEMUL: {
            uint32_t n = req.a_rows * req.a_cols;
            float *a_buf = g_malloc(n * sizeof(float));
            float *b_buf = g_malloc(n * sizeof(float));
            float *o_buf = g_malloc(n * sizeof(float));
            dma_memory_read(as, req.a_addr, a_buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_read(as, req.b_addr, b_buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_elemul(o_buf, a_buf, b_buf, n);
            dma_memory_write(as, req.out_addr, o_buf, n * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(a_buf);
            g_free(b_buf);
            g_free(o_buf);
            break;
        }

        case VACCEL_OP_ELEMADD: {
            uint32_t n = req.a_rows * req.a_cols;
            float *a_buf = g_malloc(n * sizeof(float));
            float *b_buf = g_malloc(n * sizeof(float));
            float *o_buf = g_malloc(n * sizeof(float));
            dma_memory_read(as, req.a_addr, a_buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            dma_memory_read(as, req.b_addr, b_buf, n * sizeof(float),
                            MEMTXATTRS_UNSPECIFIED);
            host_elemadd(o_buf, a_buf, b_buf, n);
            dma_memory_write(as, req.out_addr, o_buf, n * sizeof(float),
                             MEMTXATTRS_UNSPECIFIED);
            g_free(a_buf);
            g_free(b_buf);
            g_free(o_buf);
            break;
        }

        case VACCEL_OP_PING:
            /* No-op health check */
            break;

        default:
            resp.status = VACCEL_STATUS_UNSUPPORTED;
            qemu_log_mask(LOG_UNIMP,
                          "virtio-accel: unsupported op %u\n", req.op);
            break;
        }

        /* Write response to guest */
        iov_from_buf(elem->in_sg, elem->in_num, 0, &resp, sizeof(resp));
        virtqueue_push(vq, elem, sizeof(resp));
        g_free(elem);
    }

    virtio_notify(vdev, vq);
}

/* ------------------------------------------------------------------ */
/* Device lifecycle                                                   */
/* ------------------------------------------------------------------ */

static void virtio_accel_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOAccel *s = VIRTIO_ACCEL(vdev);
    memcpy(config, &s->config, sizeof(s->config));
}

static uint64_t virtio_accel_get_features(VirtIODevice *vdev, uint64_t f,
                                          Error **errp)
{
    f |= (1ULL << VIRTIO_ACCEL_F_MATMUL);
    f |= (1ULL << VIRTIO_ACCEL_F_SOFTMAX);
    f |= (1ULL << VIRTIO_ACCEL_F_RMSNORM);
    f |= (1ULL << VIRTIO_ACCEL_F_ROPE);
    f |= (1ULL << VIRTIO_ACCEL_F_SILU);
    return f;
}

static void virtio_accel_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOAccel *s = VIRTIO_ACCEL(dev);

    virtio_init(vdev, TYPE_VIRTIO_ACCEL, VIRTIO_ID_ACCEL,
                sizeof(VirtIOAccelConfig));

    s->vq = virtio_add_queue(vdev, 64, virtio_accel_handle_output);

    s->config.max_tensor_bytes = 64 * 1024 * 1024;  /* 64MB max tensor */
    s->config.num_compute_units = 1;
    s->config.features = (1 << VIRTIO_ACCEL_F_MATMUL) |
                         (1 << VIRTIO_ACCEL_F_SOFTMAX) |
                         (1 << VIRTIO_ACCEL_F_RMSNORM) |
                         (1 << VIRTIO_ACCEL_F_ROPE) |
                         (1 << VIRTIO_ACCEL_F_SILU);

    qemu_log("virtio-accel: device realized (host CPU compute)\n");
}

static void virtio_accel_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    virtio_cleanup(vdev);
}

/* ------------------------------------------------------------------ */
/* QOM type registration                                              */
/* ------------------------------------------------------------------ */

static void virtio_accel_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->desc = "Virtio Tensor Compute Accelerator";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    vdc->realize = virtio_accel_realize;
    vdc->unrealize = virtio_accel_unrealize;
    vdc->get_config = virtio_accel_get_config;
    vdc->get_features = virtio_accel_get_features;
}

static const TypeInfo virtio_accel_info = {
    .name          = TYPE_VIRTIO_ACCEL,
    .parent        = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOAccel),
    .class_init    = virtio_accel_class_init,
};

static void virtio_accel_register_types(void)
{
    type_register_static(&virtio_accel_info);
}

type_init(virtio_accel_register_types);
