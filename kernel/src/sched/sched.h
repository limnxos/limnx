#ifndef LIMNX_SCHED_H
#define LIMNX_SCHED_H

#include "sched/thread.h"

void sched_init(void);
void schedule(void);
void sched_yield(void);
void sched_add(thread_t *t);
void sched_set_smp_active(void);

/* Remove a thread from the ready queue (for SIGSTOP) */
void sched_remove(thread_t *t);

/* Block/wake a thread (for sleeping mutex, poll, etc.) */
void sched_block(thread_t *t);
void sched_wake(thread_t *t);

/* Called by timer IRQ to trigger preemption */
void sched_tick(void);

#endif
