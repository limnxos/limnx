#include "sched/thread.h"
#include "sched/sched.h"
#include "mm/kheap.h"
#include "sync/spinlock.h"
#include "serial.h"

static uint64_t next_tid = 0;
static spinlock_t thread_lock = SPINLOCK_INIT;

/* Wrapper so thread_exit() is called if the entry function returns.
 * Called from thread_trampoline (switch.asm) with entry in rdi. */
void thread_entry_wrapper(void (*entry)(void)) {
    /* Enable interrupts — schedule() keeps CLI during context_switch,
     * and new threads arrive here via thread_trampoline without going
     * through the RFLAGS restore path in schedule(). */
    __asm__ volatile ("sti");
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

    /* Initialize FPU state to default values */
    for (int i = 0; i < FPU_STATE_SIZE; i++)
        t->fpu_state[i] = 0;
    /* FCW = 0x037F: mask all x87 exceptions, double precision */
    t->fpu_state[0] = 0x7F;
    t->fpu_state[1] = 0x03;
    /* MXCSR = 0x1F80: mask all SIMD exceptions, round-to-nearest (at offset 24) */
    t->fpu_state[24] = 0x80;
    t->fpu_state[25] = 0x1F;

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

    /* Place the cpu_context_t at the top of the stack */
    cpu_context_t *ctx = (cpu_context_t *)(stack_top - sizeof(cpu_context_t));

    ctx->r15 = 0;
    ctx->r14 = 0;
    ctx->r13 = 0;
    ctx->r12 = 0;
    ctx->rbx = (uint64_t)entry;  /* thread_trampoline will move this to rdi */
    ctx->rbp = 0;
    ctx->rip = (uint64_t)thread_trampoline;  /* defined in switch.asm */

    t->context = ctx;

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
    for (;;) __asm__ volatile("hlt");
}
