#define pr_fmt(fmt) "[pipe] " fmt
#include "klog.h"

#include "ipc/pipe.h"
#include "sync/spinlock.h"

static spinlock_t pipe_lock = SPINLOCK_INIT;

pipe_t pipes[MAX_PIPES];

void pipe_lock_acquire(uint64_t *flags) {
    spin_lock_irqsave(&pipe_lock, flags);
}

void pipe_unlock_release(uint64_t flags) {
    spin_unlock_irqrestore(&pipe_lock, flags);
}
