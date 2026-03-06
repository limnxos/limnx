#include "ipc/epoll.h"
#include "syscall/syscall.h"

static epoll_instance_t epoll_table[MAX_EPOLL_INSTANCES];

int epoll_create_instance(void) {
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (!epoll_table[i].used) {
            epoll_table[i].used = 1;
            epoll_table[i].refs = 1;
            epoll_table[i].interest_count = 0;
            for (int j = 0; j < EPOLL_MAX_FDS; j++)
                epoll_table[i].interests[j].fd = -1;
            return i;
        }
    }
    return -1;
}

int epoll_ctl(int idx, int op, int fd, const epoll_event_t *event) {
    if (idx < 0 || idx >= MAX_EPOLL_INSTANCES || !epoll_table[idx].used)
        return -EINVAL;

    epoll_instance_t *ep = &epoll_table[idx];

    if (op == EPOLL_CTL_ADD) {
        /* Check for duplicate */
        for (uint32_t i = 0; i < EPOLL_MAX_FDS; i++) {
            if (ep->interests[i].fd == fd)
                return -EEXIST;
        }
        /* Find free slot */
        for (uint32_t i = 0; i < EPOLL_MAX_FDS; i++) {
            if (ep->interests[i].fd == -1) {
                ep->interests[i].fd = fd;
                ep->interests[i].events = event->events;
                ep->interests[i].data = event->data;
                ep->interest_count++;
                return 0;
            }
        }
        return -ENOBUFS;
    }

    if (op == EPOLL_CTL_MOD) {
        for (uint32_t i = 0; i < EPOLL_MAX_FDS; i++) {
            if (ep->interests[i].fd == fd) {
                ep->interests[i].events = event->events;
                ep->interests[i].data = event->data;
                return 0;
            }
        }
        return -ENOENT;
    }

    if (op == EPOLL_CTL_DEL) {
        for (uint32_t i = 0; i < EPOLL_MAX_FDS; i++) {
            if (ep->interests[i].fd == fd) {
                ep->interests[i].fd = -1;
                ep->interests[i].events = 0;
                ep->interests[i].data = 0;
                if (ep->interest_count > 0)
                    ep->interest_count--;
                return 0;
            }
        }
        return -ENOENT;
    }

    return -EINVAL;
}

void epoll_close(int idx) {
    if (idx < 0 || idx >= MAX_EPOLL_INSTANCES) return;
    epoll_instance_t *ep = &epoll_table[idx];
    if (!ep->used) return;
    if (ep->refs > 0) ep->refs--;
    if (ep->refs == 0) {
        ep->used = 0;
        ep->interest_count = 0;
    }
}

void epoll_ref(int idx) {
    if (idx < 0 || idx >= MAX_EPOLL_INSTANCES) return;
    if (epoll_table[idx].used)
        epoll_table[idx].refs++;
}

epoll_instance_t *epoll_get(int idx) {
    if (idx < 0 || idx >= MAX_EPOLL_INSTANCES) return (void *)0;
    if (!epoll_table[idx].used) return (void *)0;
    return &epoll_table[idx];
}

int epoll_index(epoll_instance_t *ep) {
    if (!ep) return -1;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (&epoll_table[i] == ep)
            return i;
    }
    return -1;
}
