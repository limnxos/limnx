#include "sched/thread.h"
#include "sched/sched.h"
#include "mm/kheap.h"
#include "sync/spinlock.h"
#include "arch/serial.h"
#include "arch/cpu.h"

static uint64_t next_tid = 0;
static spinlock_t thread_lock = SPINLOCK_INIT;

/* Wrapper so thread_exit() is called if the entry function returns.
 * Called from thread_trampoline (switch.asm) with entry in rdi. */
void thread_entry_wrapper(void (*entry)(void)) {
    /* Enable interrupts — schedule() keeps CLI during context_switch,
     * and new threads arrive here via thread_trampoline without going
     * through the RFLAGS restore path in schedule(). */
    arch_irq_enable();
    entry();
    thread_exit();
}

thread_t *thread_create(void (*entry)(void), uint64_t stack_size) {
    if (stack_size == 0)
        stack_size = THREAD_DEFAULT_STACK_SIZE;

    thread_t *t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) return NULL;

    void *stack = kmalloc(stack_size);
    if (!stack) {
        kfree(t);
        return NULL;
    }

    uint64_t tid_flags;
    spin_lock_irqsave(&thread_lock, &tid_flags);
    t->tid        = next_tid++;
    spin_unlock_irqrestore(&thread_lock, tid_flags);
    t->state      = THREAD_READY;
    t->stack_base = (uint64_t)stack;
    t->stack_size = stack_size;
    t->next       = NULL;
    t->process    = NULL;

    t->ticks_used = 0;

    /* Initialize FPU/NEON state to default values */
    for (int i = 0; i < FPU_STATE_SIZE; i++)
        t->fpu_state[i] = 0;
#if defined(__x86_64__)
    /* FCW = 0x037F: mask all x87 exceptions, double precision */
    t->fpu_state[0] = 0x7F;
    t->fpu_state[1] = 0x03;
    /* MXCSR = 0x1F80: mask all SIMD exceptions, round-to-nearest (at offset 24) */
    t->fpu_state[24] = 0x80;
    t->fpu_state[25] = 0x1F;
#endif
    /* ARM64 NEON: all zeros is valid default state */

    /*
     * Set up the initial stack so context_switch "returns" into our wrapper.
     *
     * Stack layout (growing down from top):
     *   [top - 8]  = entry function pointer (argument in rbx, used by wrapper trampoline)
     *   [top - 16] = 0 (alignment padding)
     *   ... cpu_context_t ...
     *
     * context_switch does:  pop r15..rbp, ret (pops rip)
     * So we set rip = thread_trampoline, rbx = entry, everything else = 0.
     *
     * We use a small trampoline: rdi = rbx (entry ptr), call thread_entry_wrapper.
     * But since we can't easily inline asm as a function pointer, we use a simpler
     * approach: set rip to a C trampoline that reads from a known location.
     *
     * Simplest approach: make the entry wrapper take its arg from RDI (System V ABI).
     * We set up a trampoline that moves rbx → rdi, then calls thread_entry_wrapper.
     * Or even simpler: just use a static trampoline in asm.
     *
     * Actually, simplest: the context_switch restores rbx. We set rbx = entry.
     * Then rip points to our asm trampoline which does: mov rdi, rbx; call wrapper.
     */
    uint64_t stack_top = (uint64_t)stack + stack_size;

    /* Ensure 16-byte alignment before the "return address" that context_switch pops */
    stack_top &= ~0xFULL;

    /* Place initial register frame at top of stack for context_switch to restore */
#if defined(__x86_64__)
    cpu_context_t *ctx = (cpu_context_t *)(stack_top - sizeof(cpu_context_t));
    ctx->r15 = 0;
    ctx->r14 = 0;
    ctx->r13 = 0;
    ctx->r12 = 0;
    ctx->rbx = (uint64_t)entry;  /* thread_trampoline will move this to rdi */
    ctx->rbp = 0;
    ctx->rip = (uint64_t)thread_trampoline;  /* defined in switch.asm */
    t->context = ctx;
#elif defined(__aarch64__)
    /* ARM64 context_switch pushes 6 pairs (12 regs × 8 = 96 bytes) onto stack.
     * We pre-fill the stack so ldp restores will pick up our values.
     * Stack layout (low to high, matching ldp order in context_switch):
     *   SP+0:  x29, x30   (FP, LR=thread_trampoline)
     *   SP+16: x27, x28
     *   SP+32: x25, x26
     *   SP+48: x23, x24
     *   SP+64: x21, x22
     *   SP+80: x19, x20   (x19=entry)
     */
    uint64_t frame_sp = stack_top - 96;
    uint64_t *frame = (uint64_t *)frame_sp;
    frame[0]  = 0;                             /* x29 (FP) */
    frame[1]  = (uint64_t)thread_trampoline;   /* x30 (LR) */
    frame[2]  = 0;                             /* x27 */
    frame[3]  = 0;                             /* x28 */
    frame[4]  = 0;                             /* x25 */
    frame[5]  = 0;                             /* x26 */
    frame[6]  = 0;                             /* x23 */
    frame[7]  = 0;                             /* x24 */
    frame[8]  = 0;                             /* x21 */
    frame[9]  = 0;                             /* x22 */
    frame[10] = (uint64_t)entry;               /* x19 = entry function */
    frame[11] = 0;                             /* x20 */
    t->context = (cpu_context_t *)frame_sp;
#endif

    /* NOTE: thread is NOT added to the ready queue here.
     * Callers must call sched_add(t) after they finish setting up
     * the thread (e.g., setting t->process). This prevents SMP races
     * where another CPU schedules the thread before setup is complete. */
    return t;
}

void thread_exit(void) {
    thread_t *cur = thread_get_current();
    cur->state = THREAD_DEAD;
    schedule();
    /* Never returns */
    for (;;) arch_halt();
}
