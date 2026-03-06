#ifndef LIMNX_EPOLL_H
#define LIMNX_EPOLL_H

#include <stdint.h>

#define MAX_EPOLL_INSTANCES  8
#define EPOLL_MAX_FDS        64

typedef struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed)) epoll_event_t;

typedef struct epoll_interest {
    int32_t  fd;       /* -1 = unused */
    uint32_t events;
    uint64_t data;
} epoll_interest_t;

typedef struct epoll_instance {
    uint8_t           used;
    uint32_t          refs;
    epoll_interest_t  interests[EPOLL_MAX_FDS];
    uint32_t          interest_count;
} epoll_instance_t;

/* Allocate a new epoll instance, returns index or -1 */
int epoll_create_instance(void);

/* Add/mod/del interest, returns 0 or -errno */
int epoll_ctl(int idx, int op, int fd, const epoll_event_t *event);

/* Close an instance (decrements refs, frees if 0) */
void epoll_close(int idx);

/* Increment ref count */
void epoll_ref(int idx);

/* Get instance by index, returns NULL if invalid */
epoll_instance_t *epoll_get(int idx);

/* Find index of an instance pointer, returns -1 if not found */
int epoll_index(epoll_instance_t *ep);

#endif
