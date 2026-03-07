#ifndef LIMNX_PERCPU_H
#define LIMNX_PERCPU_H

#include <stdint.h>
#include "gdt/gdt.h"
#include "sched/tss.h"

#define MAX_CPUS 8

/* Forward declaration */
struct thread;

/*
 * Per-CPU data structure.
 * The first fields have FIXED offsets that must match assembly code
 * (syscall_entry.asm, isr_stubs.asm).
 *
 * Accessed via GS segment: SWAPGS on kernel entry swaps
 * MSR_KERNEL_GS_BASE (pointing here) with user GS.
 */
typedef struct percpu {
    /* === Fixed offsets for assembly — DO NOT REORDER === */
    uint64_t  kernel_rsp;             /* offset 0   — kernel stack for SYSCALL */
    uint64_t  user_rsp_save;          /* offset 8   — user RSP save slot */
    uint64_t  self;                   /* offset 16  — self pointer (for C access) */
    uint32_t  cpu_id;                 /* offset 24  */
    uint32_t  lapic_id;              /* offset 28  */
    struct thread *current_thread;    /* offset 32  */
    struct thread *idle_thread;       /* offset 40  */
    uint8_t   signal_deliver_pending; /* offset 48  */
    uint8_t   pad[7];
    uint64_t  signal_deliver_rdi;     /* offset 56  */
    uint64_t  signal_handler_rip;     /* offset 64  — handler address for signal */
    uint64_t  signal_frame_rsp;       /* offset 72  — user RSP for signal frame */
    /* === End of assembly-visible region === */

    uint32_t  lapic_ticks_per_ms;     /* offset 80  */
    uint32_t  started;                /* offset 84  — AP signals ready */

    /* Per-CPU run queue for work-stealing scheduler */
    struct thread *rq_head;
    struct thread *rq_tail;
    uint64_t       rq_lock;           /* spinlock (volatile uint64_t) */

    /* Per-CPU GDT and TSS */
    struct gdt_entry gdt[7] __attribute__((aligned(16)));
    struct gdt_ptr   gdtp;
    tss_t            tss __attribute__((aligned(16)));
} __attribute__((aligned(64))) percpu_t;

extern percpu_t percpu_array[MAX_CPUS];
extern uint32_t cpu_count;
extern uint32_t bsp_cpu_id;

/* Read the per-CPU pointer from GS:16 (self field) */
static inline percpu_t *percpu_get(void) {
    percpu_t *p;
    __asm__ volatile ("mov %%gs:16, %0" : "=r"(p));
    return p;
}

/* Read per-CPU pointer without GS (for BSP before SWAPGS setup) */
static inline percpu_t *percpu_get_bsp(void) {
    return &percpu_array[0];
}

/* SMP initialization */
void smp_init(void);

#endif
