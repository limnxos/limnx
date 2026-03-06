#ifndef LIMNX_SPINLOCK_H
#define LIMNX_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile uint64_t locked;
} spinlock_t;

#define SPINLOCK_INIT {0}

static inline void spin_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1))
        __asm__ volatile ("pause");
}

static inline void spin_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->locked);
}

static inline void spin_lock_irqsave(spinlock_t *lock, uint64_t *flags) {
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(*flags) : : "memory");
    spin_lock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spin_unlock(lock);
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory");
}

#endif
