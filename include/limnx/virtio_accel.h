/*
 * virtio-accel protocol definitions.
 *
 * Shared between kernel driver, user-space HAL, and QEMU host backend.
 * Uses fixed-width types only.
 */
#ifndef LIMNX_VIRTIO_ACCEL_H
#define LIMNX_VIRTIO_ACCEL_H

#include <stdint.h>

/* Virtio device ID (vendor-specific range 32-63) */
#define VIRTIO_ACCEL_DEVICE_ID   48

/* Feature bits */
#define VACCEL_F_MATMUL   (1 << 0)
#define VACCEL_F_SOFTMAX  (1 << 1)
#define VACCEL_F_RMSNORM  (1 << 2)
#define VACCEL_F_ROPE     (1 << 3)
#define VACCEL_F_DEQUANT  (1 << 4)
#define VACCEL_F_SILU     (1 << 5)

/* Operation codes */
#define VACCEL_OP_MATMUL    1   /* out = A @ B                          */
#define VACCEL_OP_SOFTMAX   2   /* in-place softmax on A, size=a_cols   */
#define VACCEL_OP_RMSNORM   3   /* out = rmsnorm(A, B_weights), eps=fp0 */
#define VACCEL_OP_ROPE      4   /* in-place RoPE, pos=p0, hdim=p1      */
#define VACCEL_OP_SILU      5   /* in-place SiLU on A                   */
#define VACCEL_OP_ELEMUL    6   /* out = A * B element-wise             */
#define VACCEL_OP_ELEMADD   7   /* out = A + B element-wise             */
#define VACCEL_OP_DEQUANT   8   /* out(f32) = dequant(A), type=p0      */
#define VACCEL_OP_PING      255 /* health check, no-op                  */

/* Data types */
#define VACCEL_DTYPE_F32    0
#define VACCEL_DTYPE_F16    1

/* Status codes */
#define VACCEL_STATUS_OK          0
#define VACCEL_STATUS_UNSUPPORTED 1
#define VACCEL_STATUS_ERROR       2

/* Request descriptor (80 bytes, guest → device) */
typedef struct virtio_accel_req {
    uint32_t op;
    uint32_t id;
    uint32_t flags;        /* 0=sync */
    uint32_t dtype;
    uint64_t a_addr;       /* phys addr of input A */
    uint32_t a_rows;
    uint32_t a_cols;
    uint64_t b_addr;       /* phys addr of input B */
    uint32_t b_rows;
    uint32_t b_cols;
    uint64_t out_addr;     /* phys addr of output */
    uint32_t out_rows;
    uint32_t out_cols;
    uint32_t param0;
    uint32_t param1;
    float    fparam0;
} __attribute__((packed)) virtio_accel_req_t;

/* Response descriptor (16 bytes, device → guest) */
typedef struct virtio_accel_resp {
    uint32_t id;
    uint32_t status;
    uint64_t cycles;
} __attribute__((packed)) virtio_accel_resp_t;

/* User-space request struct (passed to SYS_ACCEL_SUBMIT) */
typedef struct accel_request {
    uint32_t op;
    uint32_t dtype;
    void    *a_data;    uint32_t a_rows, a_cols;
    void    *b_data;    uint32_t b_rows, b_cols;
    void    *out_data;  uint32_t out_rows, out_cols;
    uint32_t param0, param1;
    float    fparam0;
} accel_request_t;

/* Info struct (returned by SYS_ACCEL_INFO) */
typedef struct accel_info {
    uint32_t available;        /* 1 if accelerator present */
    uint32_t features;         /* bitmask of VACCEL_F_* */
    uint32_t max_tensor_bytes; /* max single tensor */
    uint32_t num_compute_units;
} accel_info_t;

#endif /* LIMNX_VIRTIO_ACCEL_H */
