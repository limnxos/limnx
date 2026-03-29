/*
 * Accelerator syscall handlers.
 *
 * SYS_ACCEL_SUBMIT — submit a tensor operation to the accelerator
 * SYS_ACCEL_INFO   — query accelerator capabilities
 */

#define pr_fmt(fmt) "[sys_accel] " fmt
#include "klog.h"
#include "syscall/syscall_internal.h"
#include "accel/virtio_accel_mmio.h"
#include "limnx/virtio_accel.h"
#include "mm/vmm.h"
#include "errno.h"

/*
 * SYS_ACCEL_SUBMIT(req_ptr)
 *
 * User passes pointer to accel_request_t. Kernel translates user-space
 * virtual addresses to physical, builds virtio request, submits.
 */
int64_t sys_accel_submit(uint64_t req_ptr, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (!virtio_accel_available())
        return -ENODEV;

    if (validate_user_ptr(req_ptr, sizeof(accel_request_t)) != 0)
        return -EFAULT;

    accel_request_t *ureq = (accel_request_t *)req_ptr;

    /* Validate data pointers */
    uint64_t a_size = (uint64_t)ureq->a_rows * ureq->a_cols * sizeof(float);
    uint64_t b_size = (uint64_t)ureq->b_rows * ureq->b_cols * sizeof(float);
    uint64_t o_size = (uint64_t)ureq->out_rows * ureq->out_cols * sizeof(float);

    uint64_t a_phys = 0, b_phys = 0, out_phys = 0;

    if (ureq->a_data && a_size > 0) {
        if (validate_user_ptr((uint64_t)ureq->a_data, a_size) != 0)
            return -EFAULT;
        a_phys = vmm_get_phys((uint64_t)ureq->a_data);
        if (a_phys == 0) return -EFAULT;
    }

    if (ureq->b_data && b_size > 0) {
        if (validate_user_ptr((uint64_t)ureq->b_data, b_size) != 0)
            return -EFAULT;
        b_phys = vmm_get_phys((uint64_t)ureq->b_data);
        if (b_phys == 0) return -EFAULT;
    }

    if (ureq->out_data && o_size > 0) {
        if (validate_user_ptr((uint64_t)ureq->out_data, o_size) != 0)
            return -EFAULT;
        out_phys = vmm_get_phys((uint64_t)ureq->out_data);
        if (out_phys == 0) return -EFAULT;
    }

    /* Bitcast float → uint32_t without using FP registers (ARM64 kernel) */
    uint32_t fparam0_bits;
    {
        const uint8_t *src = (const uint8_t *)&ureq->fparam0;
        uint8_t *dst = (uint8_t *)&fparam0_bits;
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
    }

    int ret = virtio_accel_submit(
        ureq->op,
        a_phys,   ureq->a_rows,   ureq->a_cols,
        b_phys,   ureq->b_rows,   ureq->b_cols,
        out_phys, ureq->out_rows, ureq->out_cols,
        ureq->param0, ureq->param1, fparam0_bits
    );

    return (int64_t)ret;
}

/*
 * SYS_ACCEL_INFO(info_ptr)
 *
 * Returns accelerator capabilities to user space.
 */
int64_t sys_accel_info(uint64_t info_ptr, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;

    if (validate_user_ptr(info_ptr, sizeof(accel_info_t)) != 0)
        return -EFAULT;

    accel_info_t *info = (accel_info_t *)info_ptr;
    info->available = virtio_accel_available() ? 1 : 0;

    virtio_accel_get_info(&info->features, &info->max_tensor_bytes,
                          &info->num_compute_units);

    return 0;
}

/*
 * SYS_ACCEL_POLL — reserved for async operations (not implemented yet)
 */
int64_t sys_accel_poll(uint64_t request_id, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)request_id; (void)a2; (void)a3; (void)a4; (void)a5;
    return -ENOSYS;
}
