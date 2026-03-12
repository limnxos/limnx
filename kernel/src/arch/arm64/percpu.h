#ifndef LIMNX_ARM64_PERCPU_H
#define LIMNX_ARM64_PERCPU_H

#include <stdint.h>

/* Forward declaration */
struct thread;

/*
 * ARM64 per-CPU data structure.
 * Accessed via TPIDR_EL1 (kernel thread pointer register).
 */
typedef struct percpu {
    uint32_t  cpu_id;
    uint32_t  started;
    struct thread *current_thread;
    struct thread *idle_thread;

    /* Per-CPU run queue */
    struct thread *rq_head;
    struct thread *rq_tail;
    uint64_t       rq_lock;

    /* Signal delivery */
    uint8_t   signal_deliver_pending;
    uint8_t   pad[7];
    uint64_t  signal_deliver_rdi;
    uint64_t  signal_handler_rip;
    uint64_t  signal_frame_rsp;
} __attribute__((aligned(64))) percpu_t;

/* Read per-CPU pointer from TPIDR_EL1 */
static inline percpu_t *percpu_get(void) {
    percpu_t *p;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}

static inline percpu_t *percpu_get_bsp(void) {
    extern percpu_t percpu_array[];
    return &percpu_array[0];
}

#endif
