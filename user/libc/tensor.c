#include "libc.h"

/* --- PRNG (LCG) --- */

uint32_t prng_next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

float prng_float(uint32_t *state) {
    uint32_t r = prng_next(state);
    /* Map to [-1.0, 1.0] */
    return ((float)(r & 0xFFFF) / 32768.0f) - 1.0f;
}

/* --- Tensor create/destroy --- */

tensor_t tensor_create(uint32_t rows, uint32_t cols) {
    tensor_t t;
    t.rows = rows;
    t.cols = cols;
    t.size = rows * cols;
    t.mmap_pages = (t.size * sizeof(float) + PAGE_SIZE - 1) / PAGE_SIZE;

    long addr = sys_mmap(t.mmap_pages);
    if (addr <= 0) {
        t.data = NULL;
        t.mmap_addr = 0;
        t.mmap_pages = 0;
        return t;
    }

    t.mmap_addr = (uint64_t)addr;
    t.data = (float *)addr;
    memset(t.data, 0, t.size * sizeof(float));
    return t;
}

void tensor_destroy(tensor_t *t) {
    if (t->mmap_addr) {
        sys_munmap(t->mmap_addr);
        t->data = NULL;
        t->mmap_addr = 0;
        t->mmap_pages = 0;
        t->rows = 0;
        t->cols = 0;
        t->size = 0;
    }
}

/* --- Fill --- */

void tensor_fill(tensor_t *t, float value) {
    for (uint32_t i = 0; i < t->size; i++)
        t->data[i] = value;
}

void tensor_fill_random(tensor_t *t, uint32_t *seed) {
    for (uint32_t i = 0; i < t->size; i++)
        t->data[i] = prng_float(seed) * 0.5f;
}

/* --- Element-wise ops --- */

void tensor_add(tensor_t *dst, const tensor_t *a, const tensor_t *b) {
    for (uint32_t i = 0; i < dst->size; i++)
        dst->data[i] = a->data[i] + b->data[i];
}

void tensor_mul(tensor_t *dst, const tensor_t *a, const tensor_t *b) {
    for (uint32_t i = 0; i < dst->size; i++)
        dst->data[i] = a->data[i] * b->data[i];
}

void tensor_scale(tensor_t *dst, const tensor_t *src, float scalar) {
    for (uint32_t i = 0; i < dst->size; i++)
        dst->data[i] = src->data[i] * scalar;
}

void tensor_add_bias(tensor_t *dst, const tensor_t *src, const tensor_t *bias) {
    for (uint32_t r = 0; r < dst->rows; r++)
        for (uint32_t c = 0; c < dst->cols; c++)
            dst->data[r * dst->cols + c] =
                src->data[r * dst->cols + c] + bias->data[c];
}

/* --- Matrix multiplication --- */

void tensor_matmul(tensor_t *dst, const tensor_t *a, const tensor_t *b) {
    /* Try accelerator first */
    if (accel_matmul(dst->data, a->data, b->data,
                     a->rows, a->cols, a->cols, b->cols) == 0)
        return;

    /* CPU fallback: dst(a.rows x b.cols) = a(a.rows x a.cols) * b(a.cols x b.cols) */
    for (uint32_t i = 0; i < dst->size; i++)
        dst->data[i] = 0.0f;

    for (uint32_t i = 0; i < a->rows; i++)
        for (uint32_t k = 0; k < a->cols; k++)
            for (uint32_t j = 0; j < b->cols; j++)
                dst->data[i * b->cols + j] +=
                    a->data[i * a->cols + k] * b->data[k * b->cols + j];
}

/* --- Activation functions --- */

void tensor_relu(tensor_t *t) {
    for (uint32_t i = 0; i < t->size; i++)
        if (t->data[i] < 0.0f)
            t->data[i] = 0.0f;
}

void tensor_softmax(tensor_t *t) {
    /* Try accelerator first */
    if (accel_softmax(t->data, t->size) == 0) return;

    /* CPU fallback */
    float max_val = t->data[0];
    for (uint32_t i = 1; i < t->size; i++)
        if (t->data[i] > max_val)
            max_val = t->data[i];

    float sum = 0.0f;
    for (uint32_t i = 0; i < t->size; i++) {
        t->data[i] = expf(t->data[i] - max_val);
        sum += t->data[i];
    }

    for (uint32_t i = 0; i < t->size; i++)
        t->data[i] /= sum;
}

/* --- Utility --- */

uint32_t tensor_argmax(const tensor_t *t) {
    uint32_t idx = 0;
    float max_val = t->data[0];
    for (uint32_t i = 1; i < t->size; i++) {
        if (t->data[i] > max_val) {
            max_val = t->data[i];
            idx = i;
        }
    }
    return idx;
}
