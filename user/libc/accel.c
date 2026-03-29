/*
 * accel.c — User-space accelerator HAL for Limnx.
 *
 * Probes for virtio-accel device via SYS_ACCEL_INFO.
 * If available, routes tensor ops through the accelerator.
 * Falls back to CPU if no device present.
 */

#include "libc.h"
#include "limnx/virtio_accel.h"

static int accel_probed;
static int accel_present;
static accel_info_t accel_hw_info;

/* Probe for accelerator — called once, lazily */
static void accel_probe(void) {
    if (accel_probed) return;
    accel_probed = 1;
    if (sys_accel_info(&accel_hw_info) == 0 && accel_hw_info.available)
        accel_present = 1;
}

int accel_available(void) {
    accel_probe();
    return accel_present;
}

int accel_get_info(accel_info_t *info) {
    accel_probe();
    if (!accel_present) return -1;
    *info = accel_hw_info;
    return 0;
}

/*
 * Accelerated matmul: out[n] = x[n] @ w[n x d] → out[d]
 * For transformer: x is (1 x n), w is (n x d), out is (1 x d)
 * Returns 0 on success, -1 on failure (caller should use CPU fallback).
 */
int accel_matmul(float *out, const float *a, const float *b,
                 uint32_t a_rows, uint32_t a_cols,
                 uint32_t b_rows, uint32_t b_cols) {
    accel_probe();
    if (!accel_present) return -1;

    accel_request_t req;
    req.op       = VACCEL_OP_MATMUL;
    req.dtype    = VACCEL_DTYPE_F32;
    req.a_data   = (void *)a;
    req.a_rows   = a_rows;
    req.a_cols   = a_cols;
    req.b_data   = (void *)b;
    req.b_rows   = b_rows;
    req.b_cols   = b_cols;
    req.out_data = (void *)out;
    req.out_rows = a_rows;
    req.out_cols = b_cols;
    req.param0   = 0;
    req.param1   = 0;
    req.fparam0  = 0.0f;

    return (int)sys_accel_submit(&req);
}

/*
 * Accelerated softmax: in-place softmax on x[size].
 */
int accel_softmax(float *x, uint32_t size) {
    accel_probe();
    if (!accel_present) return -1;

    accel_request_t req;
    req.op       = VACCEL_OP_SOFTMAX;
    req.dtype    = VACCEL_DTYPE_F32;
    req.a_data   = (void *)x;
    req.a_rows   = 1;
    req.a_cols   = size;
    req.b_data   = NULL;
    req.b_rows   = 0;
    req.b_cols   = 0;
    req.out_data = (void *)x;  /* in-place */
    req.out_rows = 1;
    req.out_cols = size;
    req.param0   = 0;
    req.param1   = 0;
    req.fparam0  = 0.0f;

    return (int)sys_accel_submit(&req);
}

/*
 * Accelerated RMS norm: out = rmsnorm(x, weight), dim elements.
 */
int accel_rmsnorm(float *out, const float *x, const float *weight,
                  uint32_t dim, float eps) {
    accel_probe();
    if (!accel_present) return -1;

    accel_request_t req;
    req.op       = VACCEL_OP_RMSNORM;
    req.dtype    = VACCEL_DTYPE_F32;
    req.a_data   = (void *)x;
    req.a_rows   = 1;
    req.a_cols   = dim;
    req.b_data   = (void *)weight;
    req.b_rows   = 1;
    req.b_cols   = dim;
    req.out_data = (void *)out;
    req.out_rows = 1;
    req.out_cols = dim;
    req.param0   = 0;
    req.param1   = 0;
    req.fparam0  = eps;

    return (int)sys_accel_submit(&req);
}

/*
 * Accelerated SiLU: in-place x = x * sigmoid(x).
 */
int accel_silu(float *x, uint32_t size) {
    accel_probe();
    if (!accel_present) return -1;

    accel_request_t req;
    req.op       = VACCEL_OP_SILU;
    req.dtype    = VACCEL_DTYPE_F32;
    req.a_data   = (void *)x;
    req.a_rows   = 1;
    req.a_cols   = size;
    req.b_data   = NULL;
    req.b_rows   = 0;
    req.b_cols   = 0;
    req.out_data = (void *)x;
    req.out_rows = 1;
    req.out_cols = size;
    req.param0   = 0;
    req.param1   = 0;
    req.fparam0  = 0.0f;

    return (int)sys_accel_submit(&req);
}
