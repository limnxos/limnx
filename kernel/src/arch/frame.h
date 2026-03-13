#ifndef LIMNX_ARCH_FRAME_H
#define LIMNX_ARCH_FRAME_H

/* Full interrupt frame type -- needed by page fault handler, device IRQ handlers */
#if defined(__x86_64__)
#include "arch/x86_64/idt.h"   /* provides interrupt_frame_t */
/* Faulting instruction pointer accessor */
#define FRAME_PC(f) ((f)->rip)
#elif defined(__aarch64__)
/* ARM64 exception frame -- matches SAVE_CONTEXT in vectors.S */
#include <stdint.h>
typedef struct interrupt_frame {
    uint64_t x[31];         /* x0-x30 */
    uint64_t elr_el1;       /* saved PC */
    uint64_t spsr_el1;      /* saved PSTATE */
    uint64_t sp_el0;        /* saved user SP */
} __attribute__((packed)) interrupt_frame_t;
/* Faulting instruction pointer accessor */
#define FRAME_PC(f) ((f)->elr_el1)
#endif

#endif
