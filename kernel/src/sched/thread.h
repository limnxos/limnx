#ifndef LIMNX_THREAD_H
#define LIMNX_THREAD_H

#include <stdint.h>

#define THREAD_DEFAULT_STACK_SIZE (32 * 1024)  /* 32 KB (loopback recursion needs depth) */
#define FPU_STATE_SIZE 512  /* FXSAVE/FXRSTOR region size */

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_DEAD,
    THREAD_STOPPED,
    THREAD_BLOCKED
};

/* Callee-saved registers + return address, saved/restored by context_switch */
#if defined(__x86_64__)
typedef struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rip;
} __attribute__((packed)) cpu_context_t;
#elif defined(__aarch64__)
/* ARM64: context_switch saves/restores callee-saved regs via SP.
 * cpu_context_t* is actually just the saved SP value (not a struct pointer).
 * We define the struct for type compatibility but it's never accessed by field. */
typedef struct cpu_context {
    uint64_t x29, x30;     /* FP, LR */
    uint64_t x27, x28;
    uint64_t x25, x26;
    uint64_t x23, x24;
    uint64_t x21, x22;
    uint64_t x19, x20;
} cpu_context_t;
#endif

/* Forward declaration */
struct process;

typedef struct thread {
    uint64_t           tid;
    enum thread_state  state;
    cpu_context_t     *context;      /* saved stack pointer */
    uint64_t           stack_base;   /* kmalloc'd stack (for freeing) */
    uint64_t           stack_size;
    struct thread     *next;         /* scheduler queue link */
    struct process    *process;      /* owning process (NULL for kernel threads) */
    uint32_t           last_cpu;     /* last CPU this thread ran on */
    uint8_t            fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    uint64_t           ticks_used;   /* accumulated CPU ticks */
    uint64_t           fs_base;      /* FS.base for TLS (per-thread) */
    void              *arch_frame;   /* ARM64: per-thread exception frame ptr */
} thread_t;

thread_t *thread_create(void (*entry)(void), uint64_t stack_size);
void      thread_exit(void);
thread_t *thread_get_current(void);

/* Assembly routines (switch.asm) */
extern void context_switch(cpu_context_t **old_ctx, cpu_context_t **new_ctx,
                           void *old_fpu_state, void *new_fpu_state);
extern void thread_trampoline(void);

#endif
