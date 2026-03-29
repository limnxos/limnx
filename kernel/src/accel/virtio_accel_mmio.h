#ifndef LIMNX_VIRTIO_ACCEL_MMIO_H
#define LIMNX_VIRTIO_ACCEL_MMIO_H

#include <stdint.h>

/* Initialize the virtio-accel MMIO driver. Returns 0 on success, -1 if no device. */
int  virtio_accel_init(void);

/* Check if an accelerator device is present. */
int  virtio_accel_available(void);

/* Submit a synchronous tensor operation. Returns 0 on success.
 * fparam0_bits is the IEEE 754 representation of a float (bitcast). */
int  virtio_accel_submit(uint32_t op, uint64_t a_phys, uint32_t a_rows, uint32_t a_cols,
                         uint64_t b_phys, uint32_t b_rows, uint32_t b_cols,
                         uint64_t out_phys, uint32_t out_rows, uint32_t out_cols,
                         uint32_t param0, uint32_t param1, uint32_t fparam0_bits);

/* Query device capabilities. */
void virtio_accel_get_info(uint32_t *features, uint32_t *max_bytes, uint32_t *n_cu);

#endif
