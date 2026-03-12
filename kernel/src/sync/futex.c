#define pr_fmt(fmt) "[futex] " fmt
#include "klog.h"

#include "sync/futex.h"
#include "sync/spinlock.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "proc/process.h"
#include "mm/vmm.h"
#include "arch/serial.h"

typedef struct futex_waiter {
    thread_t  *thread;
    uint64_t   phys_addr; /* physical address of futex word */
    uint8_t    used;
} futex_waiter_t;

static futex_waiter_t waiters[FUTEX_MAX_WAITERS];
static spinlock_t futex_lock = SPINLOCK_INIT;

void futex_init(void) {
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
        waiters[i].used = 0;
    pr_info("Futex subsystem initialized\n");
}

/* Translate user virtual address to physical using process page tables */
static uint64_t user_virt_to_phys(uint64_t virt) {
    thread_t *t = thread_get_current();
    if (!t || !t->process) return 0;
    uint64_t *pte = vmm_get_pte(t->process->cr3, virt);
    if (!pte || !(*pte & 1)) return 0;  /* PTE_PRESENT = bit 0 */
    return (*pte & 0x000FFFFFFFFFF000ULL) | (virt & 0xFFF);
}

int futex_wait(uint64_t pid, uint32_t *uaddr, uint32_t expected) {
    (void)pid;
    uint64_t phys = user_virt_to_phys((uint64_t)uaddr);
    if (!phys) return -14;  /* -EFAULT */

    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);

    /* Atomic check: if value changed, don't block */
    if (*uaddr != expected) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -11;  /* -EAGAIN */
    }

    /* Find a free waiter slot */
    int slot = -1;
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (!waiters[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -12;  /* -ENOMEM */
    }

    thread_t *self = thread_get_current();
    waiters[slot].thread = self;
    waiters[slot].phys_addr = phys;
    waiters[slot].used = 1;

    /* Set thread to BLOCKED before releasing lock (prevents missed wakeup) */
    self->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&futex_lock, flags);

    /* Yield to scheduler — we're blocked, won't be picked until woken */
    schedule();

    return 0;
}

int futex_wake(uint64_t pid, uint32_t *uaddr, uint32_t max_wake) {
    (void)pid;
    uint64_t phys = user_virt_to_phys((uint64_t)uaddr);
    if (!phys) return -14;  /* -EFAULT */

    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);

    int woken = 0;
    for (int i = 0; i < FUTEX_MAX_WAITERS && (uint32_t)woken < max_wake; i++) {
        if (waiters[i].used && waiters[i].phys_addr == phys) {
            thread_t *t = waiters[i].thread;
            waiters[i].used = 0;
            spin_unlock_irqrestore(&futex_lock, flags);
            sched_wake(t);
            spin_lock_irqsave(&futex_lock, &flags);
            woken++;
        }
    }

    spin_unlock_irqrestore(&futex_lock, flags);
    return woken;
}

void futex_cleanup_pid(uint64_t pid) {
    uint64_t flags;
    spin_lock_irqsave(&futex_lock, &flags);
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (waiters[i].used && waiters[i].thread &&
            waiters[i].thread->process &&
            waiters[i].thread->process->pid == pid) {
            thread_t *t = waiters[i].thread;
            waiters[i].used = 0;
            sched_wake(t);
        }
    }
    spin_unlock_irqrestore(&futex_lock, flags);
}
