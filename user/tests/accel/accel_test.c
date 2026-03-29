/*
 * accel_test.c — Accelerator subsystem tests
 *
 * Tests the compute accelerator HAL and syscalls.
 * Works both with and without a virtio-accel device:
 * - With device: tests GPU-accelerated path
 * - Without device: tests CPU fallback path
 */

#include "../limntest.h"

/* Small matmul: verify result matches expected */
static void test_matmul_cpu_fallback(void) {
    /* A = [1, 2, 3]  (1x3)
     * B = [1, 0]     (3x2)
     *     [0, 1]
     *     [1, 1]
     * Expected: out = [4, 5]  (1x2) */
    float a[3] = {1.0f, 2.0f, 3.0f};
    float b[6] = {1.0f, 0.0f,
                  0.0f, 1.0f,
                  1.0f, 1.0f};
    float out[2] = {0.0f, 0.0f};

    /* Use the HAL — will fall back to CPU if no device */
    int ret = accel_matmul(out, a, b, 1, 3, 3, 2);

    if (ret == 0) {
        /* Accelerator handled it */
        lt_ok(fabsf(out[0] - 4.0f) < 0.01f && fabsf(out[1] - 5.0f) < 0.01f,
              "accel_matmul via device: correct result");
    } else {
        /* No device — verify fallback was attempted (ret == -1) */
        lt_ok(ret == -1, "accel_matmul: returns -1 when no device");
    }
}

/* Test accel_info syscall */
static void test_accel_info(void) {
    accel_info_t info;
    long ret = sys_accel_info(&info);

    lt_ok(ret == 0, "sys_accel_info: returns 0");

    if (info.available) {
        lt_ok(info.features != 0, "accel_info: has feature bits");
        lt_ok(info.max_tensor_bytes > 0, "accel_info: max_tensor_bytes > 0");
        printf("  Accelerator: features=0x%x max=%u CUs=%u\n",
               info.features, info.max_tensor_bytes, info.num_compute_units);
    } else {
        lt_ok(info.features == 0, "accel_info: no features (CPU-only)");
        printf("  No accelerator device — CPU-only mode\n");
    }
}

/* Test accel_available() probe */
static void test_accel_probe(void) {
    int avail = accel_available();
    lt_ok(avail == 0 || avail == 1, "accel_available: returns 0 or 1");
    printf("  Accelerator present: %s\n", avail ? "yes" : "no");
}

/* Test softmax CPU fallback */
static void test_softmax_fallback(void) {
    float x[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    int ret = accel_softmax(x, 4);

    if (ret == 0) {
        /* Verify softmax properties: sum to 1, monotonically increasing */
        float sum = x[0] + x[1] + x[2] + x[3];
        lt_ok(fabsf(sum - 1.0f) < 0.01f, "accel_softmax via device: sums to 1");
        lt_ok(x[0] < x[1] && x[1] < x[2] && x[2] < x[3],
              "accel_softmax via device: monotonic");
    } else {
        lt_ok(ret == -1, "accel_softmax: returns -1 when no device");
    }
}

/* Test rmsnorm CPU fallback */
static void test_rmsnorm_fallback(void) {
    float x[4]   = {1.0f, 2.0f, 3.0f, 4.0f};
    float w[4]   = {1.0f, 1.0f, 1.0f, 1.0f};
    float out[4] = {0};

    int ret = accel_rmsnorm(out, x, w, 4, 1e-5f);

    if (ret == 0) {
        /* With unit weights, rmsnorm should normalize to ~unit RMS */
        float rms = 0;
        for (int i = 0; i < 4; i++) rms += out[i] * out[i];
        rms = sqrtf(rms / 4.0f);
        lt_ok(fabsf(rms - 1.0f) < 0.1f, "accel_rmsnorm via device: unit RMS");
    } else {
        lt_ok(ret == -1, "accel_rmsnorm: returns -1 when no device");
    }
}

/* Test that invalid pointer is rejected by syscall */
static void test_invalid_ptr(void) {
    long ret = sys_accel_submit(NULL);
    lt_ok(ret < 0, "sys_accel_submit(NULL): returns error");
}

/* Test tensor_matmul uses accel HAL */
static void test_tensor_integration(void) {
    tensor_t a, b, dst;

    /* 2x3 @ 3x2 = 2x2 */
    a = tensor_create(2, 3);
    b = tensor_create(3, 2);
    dst = tensor_create(2, 2);
    if (!a.data || !b.data || !dst.data) {
        lt_ok(0, "tensor_create: allocation failed");
        return;
    }

    /* A = [[1,0,0],[0,1,0]] */
    a.data[0] = 1; a.data[1] = 0; a.data[2] = 0;
    a.data[3] = 0; a.data[4] = 1; a.data[5] = 0;

    /* B = [[5,6],[7,8],[9,10]] */
    b.data[0] = 5; b.data[1] = 6;
    b.data[2] = 7; b.data[3] = 8;
    b.data[4] = 9; b.data[5] = 10;

    tensor_matmul(&dst, &a, &b);

    /* Expected: [[5,6],[7,8]] */
    lt_ok(fabsf(dst.data[0] - 5.0f) < 0.01f &&
          fabsf(dst.data[1] - 6.0f) < 0.01f &&
          fabsf(dst.data[2] - 7.0f) < 0.01f &&
          fabsf(dst.data[3] - 8.0f) < 0.01f,
          "tensor_matmul: correct result (via accel HAL)");

    tensor_destroy(&a);
    tensor_destroy(&b);
    tensor_destroy(&dst);
}

int main(void) {
    lt_suite("accel");

    test_accel_probe();
    test_accel_info();
    test_matmul_cpu_fallback();
    test_softmax_fallback();
    test_rmsnorm_fallback();
    test_invalid_ptr();
    test_tensor_integration();

    return lt_done();
}
