#define pr_fmt(fmt) "[mutex] " fmt
#include "klog.h"

#include "sync/mutex.h"
#include "sched/sched.h"

void mutex_lock(mutex_t *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    if (!m->held) {
        m->held = 1;
        m->owner = thread_get_current();
        spin_unlock_irqrestore(&m->lock, flags);
        return;
    }

    /* Mutex held — add self to wait queue and block */
    thread_t *self = thread_get_current();
    if (m->wait_count < MUTEX_WAIT_QUEUE_SIZE) {
        m->wait_queue[m->wait_count++] = self;
    }
    self->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&m->lock, flags);
    schedule();
}

void mutex_unlock(mutex_t *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    if (m->wait_count > 0) {
        /* Transfer ownership to first waiter */
        thread_t *next = m->wait_queue[0];
        /* Shift queue */
        for (uint32_t i = 1; i < m->wait_count; i++)
            m->wait_queue[i - 1] = m->wait_queue[i];
        m->wait_count--;
        m->owner = next;
        spin_unlock_irqrestore(&m->lock, flags);
        sched_wake(next);
    } else {
        m->held = 0;
        m->owner = 0;
        spin_unlock_irqrestore(&m->lock, flags);
    }
}

int mutex_trylock(mutex_t *m) {
    uint64_t flags;
    spin_lock_irqsave(&m->lock, &flags);

    if (!m->held) {
        m->held = 1;
        m->owner = thread_get_current();
        spin_unlock_irqrestore(&m->lock, flags);
        return 0;
    }

    spin_unlock_irqrestore(&m->lock, flags);
    return -1;
}
