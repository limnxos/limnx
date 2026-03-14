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
#include "arch/percpu.h"
#include "arch/frame.h"
#include "sched/thread.h"
#include <stdint.h>

/* Page fault handler from sys_mm.c — handles COW, swap-in, demand paging */
extern int page_fault_handler(uint64_t fault_addr, uint64_t err_code,
                              interrupt_frame_t *frame);

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
/* Per-CPU exception frame pointer — used by sys_fork to read user context */
extern uint64_t *arm64_exception_frame[];

void arm64_sync_handler(arm64_frame_t *frame, uint64_t esr) {
    uint32_t ec = (esr & ESR_EC_MASK) >> ESR_EC_SHIFT;

    switch (ec) {
    case ESR_EC_SVC64: {
        /* Save frame pointer for sys_fork/signal to access user context.
         * Must be per-THREAD (not per-CPU) because a blocked thread may
         * be preempted by another thread on the same CPU, clobbering
         * the per-CPU pointer before the original thread resumes. */
        percpu_t *_pc = percpu_get();
        arm64_exception_frame[_pc->cpu_id] = (uint64_t *)frame;
        {
            extern thread_t *thread_get_current(void);
            thread_t *_ct = thread_get_current();
            if (_ct) _ct->arch_frame = (void *)frame;
        }

        /* Syscall: x8=number, x0-x5=args, return in x0 */
        long ret = syscall_dispatch(
            (long)frame->x[8],
            (long)frame->x[0], (long)frame->x[1],
            (long)frame->x[2], (long)frame->x[3],
            (long)frame->x[4]);
        frame->x[0] = (uint64_t)ret;

        /* Check for pending signal delivery (mirrors x86_64 syscall_entry.asm) */
        percpu_t *pc = percpu_get();
        if (pc->signal_deliver_pending) {
            pc->signal_deliver_pending = 0;
            frame->x[0] = pc->signal_deliver_rdi;   /* signal number in x0 */
            frame->elr_el1 = pc->signal_handler_rip; /* redirect PC to handler */
            frame->sp_el0 = pc->signal_frame_rsp;    /* redirect SP to signal frame */
        }
        break;
    }
    case ESR_EC_DABT_EL0: {
        /* Data abort from user mode — try COW/swap/demand paging first */
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));

        /* Translate ARM64 ESR to x86-style err_code for page_fault_handler:
         * bit 0 (present): ISS[5] (DFSC bit 2) — translation fault vs permission fault
         * bit 1 (write): ISS[6] (WnR) — 1 if write, 0 if read
         * bit 2 (user): always 1 (EL0 abort) */
        uint32_t dfsc = esr & 0x3F;  /* Data Fault Status Code */
        int is_permission = (dfsc >= 0x0C && dfsc <= 0x0F);  /* permission fault */
        int is_write = (esr >> 6) & 1;  /* WnR bit */
        uint64_t err_code = (is_permission ? 1 : 0) | (is_write ? 2 : 0) | 4;

        if (page_fault_handler(far, err_code, (interrupt_frame_t *)frame) == 0)
            break;  /* Handled (COW, swap-in, or demand page) */

        /* Unhandled — kill the process */
        serial_printf("[fault] User data abort at PC=0x%lx FAR=0x%lx ESR=0x%lx\n",
                      frame->elr_el1, far, esr);
        frame->x[0] = syscall_dispatch(2, 139, 0, 0, 0, 0);
        break;
    }
    case ESR_EC_DABT_EL1: {
        /* Data abort from kernel mode — fatal */
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
        serial_printf("[PANIC] Kernel data abort at PC=0x%lx FAR=0x%lx ESR=0x%lx\n",
                      frame->elr_el1, far, esr);
        for (;;) __asm__ volatile ("wfi");
        break;
    }
    case ESR_EC_IABT_EL0: {
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
        serial_printf("[fault] User instruction abort at PC=0x%lx FAR=0x%lx\n",
                      frame->elr_el1, far);
        frame->x[0] = syscall_dispatch(2, 139, 0, 0, 0, 0);
        break;
    }
    case ESR_EC_IABT_EL1: {
        uint64_t far;
        __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
        serial_printf("[PANIC] Kernel instruction abort at PC=0x%lx FAR=0x%lx\n",
                      frame->elr_el1, far);
        for (;;) __asm__ volatile ("wfi");
        break;
    }
    default: {
        serial_printf("[fault] Unhandled sync exception EC=0x%x ESR=0x%lx ELR=0x%lx\n",
                      ec, esr, frame->elr_el1);
        /* Check if exception came from lower EL (EL0) — SPSR_EL1.M[3:0] == 0 */
        uint32_t spsr_m = frame->spsr_el1 & 0xF;
        if (spsr_m == 0) {
            /* EL0 process executing privileged instruction — kill it */
            serial_printf("[fault] Killing pid (EC=0x%x from EL0, PC=0x%lx)\n",
                          ec, frame->elr_el1);
            frame->x[0] = syscall_dispatch(2, 139, 0, 0, 0, 0);
        } else {
            /* Kernel exception — fatal, halt */
            serial_printf("[PANIC] Unhandled kernel exception EC=0x%x ELR=0x%lx\n",
                          ec, frame->elr_el1);
            for (;;) __asm__ volatile ("wfi");
        }
        break;
    }
    }
}

/* Forward declarations */
extern uint32_t gic_ack(void);
extern void gic_eoi(uint32_t irq);
extern void sched_tick(void);

#define GIC_TIMER_NS_EL1 30

/*
 * IRQ handler — dispatches GIC interrupts
 */
void arm64_irq_handler(arm64_frame_t *frame) {
    (void)frame;
    uint32_t irq = gic_ack();

    if (irq == GIC_TIMER_NS_EL1) {
        /* Timer interrupt — trigger preemptive scheduling */
        /* Re-arm timer (10ms) */
        extern void arch_timer_enable_sched(void);
        uint64_t freq;
        __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
        uint64_t tval = freq / 100;  /* 10ms */
        __asm__ volatile ("msr cntp_tval_el0, %0" : : "r"(tval));

        gic_eoi(irq);
        sched_tick();
    } else if (irq < 1020) {
        /* Other interrupt */
        gic_eoi(irq);
    }
    /* irq >= 1020: spurious, no EOI needed */
}
