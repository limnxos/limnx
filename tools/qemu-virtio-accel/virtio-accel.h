/*
 * virtio-accel QEMU device — header
 *
 * Custom virtio device (ID 48) for tensor compute acceleration.
 * Processes matmul, softmax, rmsnorm, etc. on the host CPU/GPU.
 */

#ifndef HW_VIRTIO_ACCEL_H
#define HW_VIRTIO_ACCEL_H

#include "hw/virtio/virtio.h"
#include "chardev/char-fe.h"

#define TYPE_VIRTIO_ACCEL "virtio-accel-device"
#define VIRTIO_ACCEL(obj) OBJECT_CHECK(VirtIOAccel, (obj), TYPE_VIRTIO_ACCEL)

#define VIRTIO_ID_ACCEL 48

/* Feature bits */
#define VIRTIO_ACCEL_F_MATMUL   0
#define VIRTIO_ACCEL_F_SOFTMAX  1
#define VIRTIO_ACCEL_F_RMSNORM  2
#define VIRTIO_ACCEL_F_ROPE     3
#define VIRTIO_ACCEL_F_DEQUANT  4
#define VIRTIO_ACCEL_F_SILU     5

/* Op codes (must match guest include/limnx/virtio_accel.h) */
#define VACCEL_OP_MATMUL    1
#define VACCEL_OP_SOFTMAX   2
#define VACCEL_OP_RMSNORM   3
#define VACCEL_OP_ROPE      4
#define VACCEL_OP_SILU      5
#define VACCEL_OP_ELEMUL    6
#define VACCEL_OP_ELEMADD   7
#define VACCEL_OP_DEQUANT   8
#define VACCEL_OP_PING      255

/* Status codes */
#define VACCEL_STATUS_OK          0
#define VACCEL_STATUS_UNSUPPORTED 1
#define VACCEL_STATUS_ERROR       2

/* Request from guest (80 bytes, packed) */
typedef struct VirtIOAccelReq {
    uint32_t op;
    uint32_t id;
    uint32_t flags;
    uint32_t dtype;
    uint64_t a_addr;
    uint32_t a_rows;
    uint32_t a_cols;
    uint64_t b_addr;
    uint32_t b_rows;
    uint32_t b_cols;
    uint64_t out_addr;
    uint32_t out_rows;
    uint32_t out_cols;
    uint32_t param0;
    uint32_t param1;
    float    fparam0;
} QEMU_PACKED VirtIOAccelReq;

/* Response to guest (16 bytes, packed) */
typedef struct VirtIOAccelResp {
    uint32_t id;
    uint32_t status;
    uint64_t cycles;
} QEMU_PACKED VirtIOAccelResp;

/* Device config (read by guest at MMIO offset 0x100) */
typedef struct VirtIOAccelConfig {
    uint32_t max_tensor_bytes;
    uint32_t num_compute_units;
    uint32_t features;
} VirtIOAccelConfig;

/* Device state */
typedef struct VirtIOAccel {
    VirtIODevice parent_obj;
    VirtQueue *vq;
    VirtIOAccelConfig config;
} VirtIOAccel;

#endif /* HW_VIRTIO_ACCEL_H */
