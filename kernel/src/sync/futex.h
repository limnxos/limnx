#pragma once
#include <stdint.h>

#define FUTEX_BUCKETS     64
#define FUTEX_MAX_WAITERS 64

/* futex_wait: block if *uaddr == expected. Returns 0 on wake, -EAGAIN if mismatch.
 * Uses physical address of uaddr as key — works across fork with shared memory. */
int futex_wait(uint64_t pid, uint32_t *uaddr, uint32_t expected);

/* futex_wake: wake up to max_wake threads waiting on uaddr. Returns number woken. */
int futex_wake(uint64_t pid, uint32_t *uaddr, uint32_t max_wake);

void futex_init(void);

/* Clean up all waiters belonging to a process (called on process exit) */
void futex_cleanup_pid(uint64_t pid);
