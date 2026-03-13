#define pr_fmt(fmt) "[sched] " fmt
#include "klog.h"

#include "sched/sched.h"
#include "arch/syscall_arch.h"
#include "proc/process.h"
#include "mm/kheap.h"
#include "mm/vmm.h"
#include "sync/spinlock.h"
#include "syscall/syscall.h"
#include "arch/percpu.h"
#include "blk/bcache.h"
#include "net/tcp.h"
#include "ipc/infer_svc.h"
#include "arch/serial.h"
#include "arch/cpu.h"
#include "arch/paging.h"

/* Per-CPU current_thread and idle_thread are in percpu_t.
 * For backward compatibility during early boot (before SMP init),
 * we keep static fallbacks. Once smp_init() runs, the percpu fields
 * are authoritative. */
static thread_t *bsp_current_thread;
static thread_t *bsp_idle_thread;

/* Shared run queue — used before SMP is active */
static thread_t *ready_head;
static thread_t *ready_tail;

/*
 * Lock ordering: sched_lock is at level 1 (highest priority).
 * This is the top of the lock hierarchy — no other lock may be
 * held when acquiring sched_lock.
 * Must NOT hold: (nothing — this is the highest-priority lock)
 * May call kmalloc while holding this lock: NO
 * May call pmm_alloc while holding this lock: NO
 */
static spinlock_t sched_lock = SPINLOCK_INIT;

/* Flag: are we running in SMP mode? Set by smp_init. */
static volatile int smp_active = 0;

/* Dead thread list — threads are linked here when they die in schedule(),
 * and reaped (stack + struct freed) in reap_dead(). */
static thread_t *dead_head = NULL;
static spinlock_t dead_lock = SPINLOCK_INIT;

/* Get current thread: per-CPU if SMP, else static */
static inline thread_t *get_current(void) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        return pc->current_thread;
    }
    return bsp_current_thread;
}

static inline void set_current(thread_t *t) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        pc->current_thread = t;
    }
    bsp_current_thread = t;
}

static inline thread_t *get_idle(void) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        return pc->idle_thread;
    }
    return bsp_idle_thread;
}

/* --- Dead thread reaping --- */

static void link_dead(thread_t *t) {
    spin_lock(&dead_lock);
    t->next = dead_head;
    dead_head = t;
    spin_unlock(&dead_lock);
}

static void reap_dead(void) {
    if (!dead_head)
        return;
    spin_lock(&dead_lock);
    while (dead_head) {
        thread_t *t = dead_head;
        dead_head = t->next;
        spin_unlock(&dead_lock);
        if (t->stack_base)
            kfree((void *)t->stack_base);
        kfree(t);
        spin_lock(&dead_lock);
    }
    spin_unlock(&dead_lock);
}

/* --- Shared queue ops (pre-SMP) --- */

static void enqueue(thread_t *t) {
    t->next = NULL;
    if (ready_tail) {
        ready_tail->next = t;
        ready_tail = t;
    } else {
        ready_head = t;
        ready_tail = t;
    }
}

static thread_t *dequeue(void) {
    if (!ready_head)
        return NULL;
    thread_t *t = ready_head;
    ready_head = t->next;
    if (!ready_head)
        ready_tail = NULL;
    t->next = NULL;
    return t;
}

/* --- Per-CPU queue ops (SMP) --- */

static void local_enqueue(percpu_t *pc, thread_t *t) {
    t->next = NULL;
    if (pc->rq_tail) {
        pc->rq_tail->next = t;
        pc->rq_tail = t;
    } else {
        pc->rq_head = t;
        pc->rq_tail = t;
    }
}

static thread_t *local_dequeue(percpu_t *pc) {
    thread_t *t = pc->rq_head;
    if (!t)
        return NULL;
    pc->rq_head = t->next;
    if (!pc->rq_head)
        pc->rq_tail = NULL;
    t->next = NULL;
    return t;
}

/* Steal one thread from another CPU's queue tail */
static thread_t *steal_from_tail(percpu_t *vpc) {
    if (!vpc->rq_head)
        return NULL;
    /* Only one entry? */
    if (vpc->rq_head == vpc->rq_tail) {
        thread_t *t = vpc->rq_head;
        vpc->rq_head = NULL;
        vpc->rq_tail = NULL;
        t->next = NULL;
        return t;
    }
    /* Walk to second-to-last */
    thread_t *prev = vpc->rq_head;
    while (prev->next != vpc->rq_tail)
        prev = prev->next;
    thread_t *t = vpc->rq_tail;
    vpc->rq_tail = prev;
    prev->next = NULL;
    t->next = NULL;
    return t;
}

static int local_remove(percpu_t *pc, thread_t *t) {
    if (pc->rq_head == t) {
        pc->rq_head = t->next;
        if (pc->rq_tail == t) pc->rq_tail = NULL;
        t->next = NULL;
        return 1;  /* found */
    } else {
        thread_t *prev = pc->rq_head;
        while (prev && prev->next != t) prev = prev->next;
        if (prev) {
            prev->next = t->next;
            if (pc->rq_tail == t) pc->rq_tail = prev;
            t->next = NULL;
            return 1;  /* found */
        }
    }
    return 0;  /* not found */
}

/* --- Common schedule body --- */

static void do_switch(thread_t *old, thread_t *next, uint64_t flags) {
    /* Update kernel stack for this thread */
    if (next->stack_base && next->stack_size) {
        uint64_t stack_top = next->stack_base + next->stack_size;
#if defined(__x86_64__)
        if (smp_active) {
            percpu_t *pc = percpu_get();
            pc->tss.rsp0 = stack_top;
            pc->kernel_rsp = stack_top;
        } else {
            arch_set_kernel_stack(stack_top);
        }
#else
        arch_set_kernel_stack(stack_top);
#endif
    }

    /* Switch address space if the new thread belongs to a different process.
     * For DEAD threads, cr3 was already zeroed by sys_exit — don't access
     * old->process (may be freed by parent's waitpid on another CPU).
     * Safety: if a process's cr3 is 0 (freed by sys_exit), fall back to
     * kernel PML4 to avoid loading CR3=0 which would triple-fault. */
    uint64_t old_cr3 = (old->state == THREAD_DEAD) ? vmm_get_kernel_pml4()
                     : (old->process ? old->process->cr3 : vmm_get_kernel_pml4());
    uint64_t new_cr3 = next->process ? next->process->cr3 : vmm_get_kernel_pml4();
    if (old_cr3 == 0) old_cr3 = vmm_get_kernel_pml4();
    if (new_cr3 == 0) new_cr3 = vmm_get_kernel_pml4();
    if (old_cr3 != new_cr3)
        arch_switch_address_space(new_cr3);

    /* Hold the lock THROUGH context_switch. This prevents another CPU from
     * dequeuing and switching to 'old' before its context is saved.
     * After context_switch, we're on the resumed thread's stack; release
     * the lock on the new thread's behalf. */

    if (old != next) {
        /* Save/restore FS.base (TLS) across context switch */
        old->fs_base = arch_get_tls_base();
        context_switch(&old->context, &next->context,
                       old->fpu_state, next->fpu_state);
        /* After switch, 'next' is now the current thread.
         * Restore its FS.base. Note: after context_switch, local vars
         * belong to the resumed thread, so we use thread_get_current(). */
        {
            thread_t *cur = thread_get_current();
            arch_set_tls_base(cur->fs_base);
        }
    }

    /* Now on the resumed thread's stack. Release the lock.
     * In SMP mode, the per-CPU rq_lock is released.
     * In single-CPU mode, sched_lock is released. */
    if (smp_active) {
        percpu_t *pc = percpu_get();
        spin_unlock((spinlock_t *)&pc->rq_lock);
    } else {
        spin_unlock(&sched_lock);
    }

    /* Restore interrupt state from the resumed thread's saved flags.
     * After context_switch, 'flags' is this thread's stack-local variable
     * from when it previously entered schedule(). */
    arch_irq_restore(flags);
}

/* --- Idle loop --- */

static void idle_loop(void) {
    for (;;) {
        arch_irq_enable();
        arch_halt();
    }
}

/* --- Init --- */

void sched_init(void) {
    ready_head = NULL;
    ready_tail = NULL;

    /* Create the idle thread (not added to queue by thread_create) */
    bsp_idle_thread = thread_create(idle_loop, 0);
    if (!bsp_idle_thread) {
        panic("could not create idle thread");
    }

    /* Adopt the current execution (kmain) as thread 0 */
    thread_t *kmain_thread = (thread_t *)kmalloc(sizeof(thread_t));
    if (!kmain_thread) {
        panic("could not allocate kmain thread");
    }
    kmain_thread->tid        = 0;
    kmain_thread->state      = THREAD_RUNNING;
    kmain_thread->context    = NULL;  /* will be saved on first switch */
    kmain_thread->stack_base = 0;     /* don't free — it's the boot stack */
    kmain_thread->stack_size = 0;
    kmain_thread->next       = NULL;
    kmain_thread->process    = NULL;
    kmain_thread->last_cpu   = 0;

    /* Capture current FPU state into kmain_thread */
    arch_fpu_save(kmain_thread->fpu_state);

    bsp_current_thread = kmain_thread;

    pr_info("Scheduler initialized\n");
}

void sched_set_smp_active(void) {
    smp_active = 1;
    /* Copy BSP idle thread to percpu so get_idle() works in SMP mode */
    percpu_t *pc = percpu_get();
    if (pc && !pc->idle_thread)
        pc->idle_thread = bsp_idle_thread;

    /* Migrate shared queue → CPU 0's per-CPU queue */
    while (ready_head) {
        thread_t *t = dequeue();
        if (t)
            local_enqueue(pc, t);
    }
}

/* --- Add / Remove --- */

void sched_add(thread_t *t) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        uint64_t flags;
        spin_lock_irqsave((spinlock_t *)&pc->rq_lock, &flags);
        local_enqueue(pc, t);
        spin_unlock_irqrestore((spinlock_t *)&pc->rq_lock, flags);
    } else {
        uint64_t flags;
        spin_lock_irqsave(&sched_lock, &flags);
        enqueue(t);
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

int sched_remove(thread_t *t) {
    int found = 0;
    if (smp_active) {
        /* Search all per-CPU queues (rare — only for SIGKILL/SIGSTOP) */
        for (uint32_t i = 0; i < cpu_count; i++) {
            percpu_t *pc = &percpu_array[i];
            uint64_t flags;
            spin_lock_irqsave((spinlock_t *)&pc->rq_lock, &flags);
            if (local_remove(pc, t))
                found = 1;
            spin_unlock_irqrestore((spinlock_t *)&pc->rq_lock, flags);
            if (found) break;
        }
    } else {
        uint64_t flags;
        spin_lock_irqsave(&sched_lock, &flags);
        if (ready_head == t) {
            ready_head = t->next;
            if (ready_tail == t) ready_tail = NULL;
            t->next = NULL;
            found = 1;
        } else {
            thread_t *prev = ready_head;
            while (prev && prev->next != t) prev = prev->next;
            if (prev) {
                prev->next = t->next;
                if (ready_tail == t) ready_tail = prev;
                t->next = NULL;
                found = 1;
            }
        }
        spin_unlock_irqrestore(&sched_lock, flags);
    }
    return found;
}

/* --- Schedule (SMP path) --- */

static void schedule_smp(void) {
    uint64_t flags;
    percpu_t *pc = percpu_get();
    spinlock_t *my_lock = (spinlock_t *)&pc->rq_lock;

    spin_lock_irqsave(my_lock, &flags);

    reap_dead();

    thread_t *current_thread = pc->current_thread;
    thread_t *idle_thread = pc->idle_thread;

    /* Try local dequeue */
    thread_t *next = local_dequeue(pc);

    if (!next) {
        /* Try work stealing from other CPUs */
        spin_unlock(my_lock);

        for (uint32_t i = 0; i < cpu_count; i++) {
            if (i == pc->cpu_id) continue;
            percpu_t *vpc = &percpu_array[i];
            if (!vpc->started) continue;
            spinlock_t *vlock = (spinlock_t *)&vpc->rq_lock;
            spin_lock(vlock);
            next = steal_from_tail(vpc);
            spin_unlock(vlock);
            if (next) break;
        }

        /* Re-acquire local lock */
        spin_lock(my_lock);

        /* Check local queue again in case something was added while stealing */
        if (!next)
            next = local_dequeue(pc);
    }

    if (!next) {
        /* Nothing anywhere */
        if (current_thread == idle_thread || current_thread->state == THREAD_DEAD) {
            next = idle_thread;
        } else {
            /* Only thread — keep running */
            spin_unlock(my_lock);
            arch_irq_restore(flags);
            return;
        }
    }

    thread_t *old = current_thread;

    /* If old thread is still running (not dead/blocked/stopped), put it back */
    if (old->state == THREAD_RUNNING && old != idle_thread) {
        old->state = THREAD_READY;
        local_enqueue(pc, old);
    } else if (old == idle_thread && old->state == THREAD_RUNNING) {
        old->state = THREAD_READY;
        /* Don't enqueue idle — it's managed separately */
    }
    /* THREAD_STOPPED and THREAD_BLOCKED threads are NOT re-enqueued */

    /* Link dead threads onto dead list for reap_dead() */
    if (old->state == THREAD_DEAD && old != idle_thread) {
        /* Set exited=1 for SIGKILL'd threads (normal exit sets it in sys_exit).
         * Null the process pointer to prevent use-after-free: once exited=1,
         * the parent's waitpid may kfree the process on another CPU.
         * Wake any waiter inline (can't call sched_wake — we hold rq_lock). */
        if (old->process) {
            thread_t *waiter = old->process->wait_thread;
            old->process->wait_thread = NULL;
            old->process->exited = 1;
            old->process = NULL;
            if (waiter && waiter->state == THREAD_BLOCKED) {
                waiter->state = THREAD_READY;
                local_enqueue(pc, waiter);
            }
        }
        link_dead(old);
    }

    /* If the dequeued thread was killed while in the queue (SIGKILL set
     * THREAD_DEAD after we dequeued it, or between dequeue and here),
     * don't schedule it — handle it as dead immediately. */
    if (next->state == THREAD_DEAD && next != idle_thread) {
        if (next->process) {
            thread_t *waiter = next->process->wait_thread;
            next->process->wait_thread = NULL;
            next->process->exited = 1;
            next->process = NULL;
            if (waiter && waiter->state == THREAD_BLOCKED) {
                waiter->state = THREAD_READY;
                local_enqueue(pc, waiter);
            }
        }
        link_dead(next);
        /* Try to find another thread */
        next = local_dequeue(pc);
        if (!next)
            next = idle_thread;
    }

    next->state = THREAD_RUNNING;
    next->last_cpu = pc->cpu_id;
    pc->current_thread = next;
    bsp_current_thread = next;  /* keep bsp static in sync */

    do_switch(old, next, flags);
}

/* --- Schedule (single-CPU path) --- */

static void schedule_single(void) {
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);

    reap_dead();

    thread_t *current_thread = bsp_current_thread;
    thread_t *idle_thread = bsp_idle_thread;
    thread_t *next = dequeue();

    if (!next) {
        /* Nothing in ready queue */
        if (current_thread == idle_thread || current_thread->state == THREAD_DEAD) {
            /* Already idle or dead — switch to idle */
            next = idle_thread;
        } else {
            /* Only thread — keep running */
            spin_unlock_irqrestore(&sched_lock, flags);
            return;
        }
    }

    thread_t *old = current_thread;

    /* If old thread is still running (not dead/blocked/stopped), put it back */
    if (old->state == THREAD_RUNNING && old != idle_thread) {
        old->state = THREAD_READY;
        enqueue(old);
    } else if (old == idle_thread && old->state == THREAD_RUNNING) {
        old->state = THREAD_READY;
        /* Don't enqueue idle — it's managed separately */
    }
    /* THREAD_STOPPED and THREAD_BLOCKED threads are NOT re-enqueued */

    /* Link dead threads onto dead list for reap_dead() */
    if (old->state == THREAD_DEAD && old != idle_thread) {
        if (old->process) {
            thread_t *waiter = old->process->wait_thread;
            old->process->wait_thread = NULL;
            old->process->exited = 1;
            old->process = NULL;
            if (waiter && waiter->state == THREAD_BLOCKED) {
                waiter->state = THREAD_READY;
                enqueue(waiter);
            }
        }
        link_dead(old);
    }

    /* If the dequeued thread was killed while in the queue, handle as dead */
    if (next->state == THREAD_DEAD && next != idle_thread) {
        if (next->process) {
            thread_t *waiter = next->process->wait_thread;
            next->process->wait_thread = NULL;
            next->process->exited = 1;
            next->process = NULL;
            if (waiter && waiter->state == THREAD_BLOCKED) {
                waiter->state = THREAD_READY;
                enqueue(waiter);
            }
        }
        link_dead(next);
        next = dequeue();
        if (!next)
            next = idle_thread;
    }

    next->state = THREAD_RUNNING;
    bsp_current_thread = next;

    do_switch(old, next, flags);
}

void schedule(void) {
    if (smp_active)
        schedule_smp();
    else
        schedule_single();
}

/* --- Block / Wake --- */

void sched_block(thread_t *t) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        uint64_t flags;
        spin_lock_irqsave((spinlock_t *)&pc->rq_lock, &flags);
        t->state = THREAD_BLOCKED;
        spin_unlock_irqrestore((spinlock_t *)&pc->rq_lock, flags);
    } else {
        uint64_t flags;
        spin_lock_irqsave(&sched_lock, &flags);
        t->state = THREAD_BLOCKED;
        spin_unlock_irqrestore(&sched_lock, flags);
    }
    schedule();
}

void sched_wake(thread_t *t) {
    if (smp_active) {
        /* Enqueue on current CPU's queue */
        percpu_t *pc = percpu_get();
        uint64_t flags;
        spin_lock_irqsave((spinlock_t *)&pc->rq_lock, &flags);
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            local_enqueue(pc, t);
        }
        spin_unlock_irqrestore((spinlock_t *)&pc->rq_lock, flags);
    } else {
        uint64_t flags;
        spin_lock_irqsave(&sched_lock, &flags);
        if (t->state == THREAD_BLOCKED) {
            t->state = THREAD_READY;
            enqueue(t);
        }
        spin_unlock_irqrestore(&sched_lock, flags);
    }
}

void sched_yield(void) {
    schedule();
}

static uint32_t flush_counter = 0;

void sched_tick(void) {
    thread_t *cur = get_current();
    if (cur) {
        cur->ticks_used++;
        /* Check CPU time limit */
        if (cur->process && cur->process->rlimit_cpu_ticks > 0 &&
            cur->ticks_used >= cur->process->rlimit_cpu_ticks) {
            process_deliver_signal(cur->process, SIGKILL);
        }
    }
    /* Periodic TCP timer check every 500 ticks (~5 seconds), CPU 0 only.
     * bcache_flush is NOT called here because virtio_blk_write needs
     * interrupts enabled (calls sched_yield), but sched_tick runs from
     * the timer ISR with interrupts disabled. bcache flushes on eviction
     * and can be called explicitly from process context if needed. */
    flush_counter++;
    if (flush_counter >= 500) {
        flush_counter = 0;
        if (!smp_active || (smp_active && percpu_get()->cpu_id == 0)) {
            tcp_timer_check();
            infer_health_check();
            infer_queue_expire();
            infer_cache_expire();
            infer_async_expire();
        }
    }
    schedule();
}

/* Called from thread_trampoline to release the scheduler lock
 * that was held across context_switch for new threads. */
void sched_unlock_after_switch(void) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        spin_unlock((spinlock_t *)&pc->rq_lock);
    } else {
        spin_unlock(&sched_lock);
    }
    arch_irq_enable();
}

int sched_has_pending_signal(void) {
    thread_t *t = get_current();
    if (t && t->process) {
        return (t->process->pending_signals & ~t->process->signal_mask) ? 1 : 0;
    }
    return 0;
}

thread_t *thread_get_current(void) {
    if (smp_active) {
        percpu_t *pc = percpu_get();
        return pc->current_thread;
    }
    return bsp_current_thread;
}
