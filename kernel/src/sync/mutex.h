#ifndef LIMNX_MUTEX_H
#define LIMNX_MUTEX_H

#include <stdint.h>
#include "sync/spinlock.h"
#include "sched/thread.h"

#define MUTEX_WAIT_QUEUE_SIZE 8

typedef struct mutex {
    spinlock_t  lock;
    uint8_t     held;
    thread_t   *owner;
    thread_t   *wait_queue[MUTEX_WAIT_QUEUE_SIZE];
    uint32_t    wait_count;
} mutex_t;

#define MUTEX_INIT { .lock = SPINLOCK_INIT, .held = 0, .owner = 0, .wait_count = 0 }

void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);
int  mutex_trylock(mutex_t *m);  /* returns 0 on success, -1 if held */

#endif
