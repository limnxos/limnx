#define pr_fmt(fmt) "[eventfd] " fmt
#include "klog.h"

#include "ipc/eventfd.h"
#include "sched/sched.h"
#include "sync/spinlock.h"

static eventfd_t eventfds[MAX_EVENTFDS];

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t eventfd_lock = SPINLOCK_INIT;

int eventfd_alloc(uint32_t flags) {
    uint64_t iflags;
    spin_lock_irqsave(&eventfd_lock, &iflags);

    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (!eventfds[i].used) {
            eventfds[i].used = 1;
            eventfds[i].counter = 0;
            eventfds[i].refs = 1;
            eventfds[i].flags = flags;
            spin_unlock_irqrestore(&eventfd_lock, iflags);
            return i;
        }
    }

    spin_unlock_irqrestore(&eventfd_lock, iflags);
    pr_err("slot table full\n");
    return -1;
}

int eventfd_read(int idx, uint64_t *value, int nonblock) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return -1;

    eventfd_t *efd = &eventfds[idx];

    for (;;) {
        uint64_t iflags;
        spin_lock_irqsave(&eventfd_lock, &iflags);

        if (efd->counter > 0) {
            if (efd->flags & EFD_SEMAPHORE) {
                *value = 1;
                efd->counter--;
            } else {
                *value = efd->counter;
                efd->counter = 0;
            }
            spin_unlock_irqrestore(&eventfd_lock, iflags);
            return 8;
        }

        spin_unlock_irqrestore(&eventfd_lock, iflags);

        if (nonblock) return -1;
        if (!efd->used) return -1;
        sched_yield();
    }
}

int eventfd_write(int idx, uint64_t value) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return -1;

    uint64_t iflags;
    spin_lock_irqsave(&eventfd_lock, &iflags);
    eventfds[idx].counter += value;
    spin_unlock_irqrestore(&eventfd_lock, iflags);
    return 8;
}

void eventfd_close(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return;

    uint64_t iflags;
    spin_lock_irqsave(&eventfd_lock, &iflags);

    eventfd_t *efd = &eventfds[idx];
    if (efd->refs > 0)
        efd->refs--;
    if (efd->refs == 0) {
        efd->used = 0;
        efd->counter = 0;
    }

    spin_unlock_irqrestore(&eventfd_lock, iflags);
}

void eventfd_ref(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return;

    uint64_t iflags;
    spin_lock_irqsave(&eventfd_lock, &iflags);
    eventfds[idx].refs++;
    spin_unlock_irqrestore(&eventfd_lock, iflags);
}

eventfd_t *eventfd_get(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS)
        return NULL;
    if (!eventfds[idx].used)
        return NULL;
    return &eventfds[idx];
}

int eventfd_index(const eventfd_t *efd) {
    for (int i = 0; i < MAX_EVENTFDS; i++)
        if (eventfds[i].used && &eventfds[i] == efd)
            return i;
    return -1;
}

int eventfd_readable(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return 0;
    return eventfds[idx].counter > 0;
}
