#include "ipc/eventfd.h"
#include "sched/sched.h"

static eventfd_t eventfds[MAX_EVENTFDS];

int eventfd_alloc(uint32_t flags) {
    for (int i = 0; i < MAX_EVENTFDS; i++) {
        if (!eventfds[i].used) {
            eventfds[i].used = 1;
            eventfds[i].counter = 0;
            eventfds[i].refs = 1;
            eventfds[i].flags = flags;
            return i;
        }
    }
    return -1;
}

int eventfd_read(int idx, uint64_t *value, int nonblock) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return -1;

    eventfd_t *efd = &eventfds[idx];

    while (efd->counter == 0) {
        if (nonblock) return -1;
        sched_yield();
        if (!efd->used) return -1;
    }

    if (efd->flags & EFD_SEMAPHORE) {
        *value = 1;
        efd->counter--;
    } else {
        *value = efd->counter;
        efd->counter = 0;
    }
    return 8;
}

int eventfd_write(int idx, uint64_t value) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return -1;

    eventfds[idx].counter += value;
    return 8;
}

void eventfd_close(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return;

    eventfd_t *efd = &eventfds[idx];
    if (efd->refs > 0)
        efd->refs--;
    if (efd->refs == 0) {
        efd->used = 0;
        efd->counter = 0;
    }
}

void eventfd_ref(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return;
    eventfds[idx].refs++;
}

eventfd_t *eventfd_get(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS)
        return (void *)0;
    if (!eventfds[idx].used)
        return (void *)0;
    return &eventfds[idx];
}

int eventfd_readable(int idx) {
    if (idx < 0 || idx >= MAX_EVENTFDS || !eventfds[idx].used)
        return 0;
    return eventfds[idx].counter > 0;
}
