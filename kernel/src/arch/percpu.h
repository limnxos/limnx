#ifndef LIMNX_ARCH_PERCPU_H
#define LIMNX_ARCH_PERCPU_H

/*
 * Per-CPU data dispatch header.
 *
 * Each architecture defines percpu_t (full struct) and provides:
 *   percpu_get()     — get current CPU's percpu_t* (inline asm)
 *   percpu_get_bsp() — get BSP percpu_t* (safe before asm setup)
 *
 * Required fields in percpu_t (accessed by arch-independent code):
 *   .cpu_id, .current_thread, .idle_thread,
 *   .rq_head, .rq_tail, .rq_lock, .started,
 *   .signal_deliver_pending, .signal_deliver_rdi,
 *   .signal_handler_rip, .signal_frame_rsp
 */

#include <stdint.h>

#define MAX_CPUS 8

#if defined(__x86_64__)
#include "arch/x86_64/percpu.h"
#elif defined(__aarch64__)
#include "arch/arm64/percpu.h"
#else
#error "Unsupported architecture"
#endif

extern percpu_t percpu_array[MAX_CPUS];
extern uint32_t cpu_count;
extern uint32_t bsp_cpu_id;

#endif
