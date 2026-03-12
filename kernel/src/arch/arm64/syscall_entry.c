/*
 * ARM64 SVC (syscall) entry and exception dispatch
 *
 * Corresponds to x86_64 syscall_entry.asm.
 *
 * On ARM64, syscalls arrive via SVC instruction which triggers a
 * Synchronous exception at EL1. The ESR_EL1 register contains the
 * exception class (EC) which tells us if it's an SVC call.
 *
 * Syscall convention (matches x86_64):
 *   x8  = syscall number
 *   x0  = arg1, x1 = arg2, x2 = arg3, x3 = arg4, x4 = arg5, x5 = arg6
 *   Return value in x0
 */

#include "arch/serial.h"
#include <stdint.h>

/* ESR_EL1 exception class field */
#define ESR_EC_SHIFT    26
#define ESR_EC_MASK     (0x3FU << ESR_EC_SHIFT)
#define ESR_EC_SVC64    0x15    /* SVC from AArch64 */
#define ESR_EC_DABT_EL1 0x25   /* Data abort from current EL */
#define ESR_EC_DABT_EL0 0x24   /* Data abort from lower EL */
#define ESR_EC_IABT_EL1 0x21   /* Instruction abort from current EL */
#define ESR_EC_IABT_EL0 0x20   /* Instruction abort from lower EL */

/* ARM64 exception frame — matches SAVE_CONTEXT in vectors.S */
typedef struct arm64_frame {
    uint64_t x[31];         /* x0-x30 */
    uint64_t elr_el1;       /* saved PC */
    uint64_t spsr_el1;      /* saved PSTATE */
    uint64_t sp_el0;        /* saved user SP */
} arm64_frame_t;

/* External: kernel syscall dispatch (provided by full kernel build) */
long __attribute__((weak)) syscall_dispatch(long num, long a1, long a2,
                                            long a3, long a4, long a5) {
    (void)num; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return -1;  /* stub — full kernel provides real implementation */
}

/*
 * Synchronous exception handler — dispatches SVC, data/instruction aborts
 */
void arm64_sync_handler(arm64_frame_t *frame, uint64_t esr) {
    uint32_t ec = (esr & ESR_EC_MASK) >> ESR_EC_SHIFT;

    switch (ec) {
    case ESR_EC_SVC64: {
        /* Syscall: x8=number, x0-x5=args, return in x0 */
        long ret = syscall_dispatch(
            (long)frame->x[8],
            (long)frame->x[0], (long)frame->x[1],
            (long)frame->x[2], (long)frame->x[3],
            (long)frame->x[4]);
        frame->x[0] = (uint64_t)ret;
        break;
    }
    case ESR_EC_DABT_EL0:
    case ESR_EC_DABT_EL1: {
        /* Data abort — page fault equivalent */
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
        serial_printf("[fault] Data abort at PC=0x%lx FAR=0x%lx ESR=0x%lx\n",
                      frame->elr_el1, far, esr);
        /* TODO: call page fault handler */
        break;
    }
    case ESR_EC_IABT_EL0:
    case ESR_EC_IABT_EL1: {
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
        serial_printf("[fault] Instruction abort at PC=0x%lx FAR=0x%lx\n",
                      frame->elr_el1, far);
        break;
    }
    default:
        serial_printf("[fault] Unhandled sync exception EC=0x%x ESR=0x%lx ELR=0x%lx\n",
                      ec, esr, frame->elr_el1);
        break;
    }
}

/*
 * IRQ handler — dispatches GIC interrupts
 */
void arm64_irq_handler(arm64_frame_t *frame) {
    (void)frame;
    /* TODO: gic_ack() → dispatch → gic_eoi() */
    serial_puts("[irq]  ARM64 IRQ received (stub)\n");
}
