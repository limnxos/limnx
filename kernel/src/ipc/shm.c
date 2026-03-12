#define pr_fmt(fmt) "[shm] " fmt
#include "klog.h"

#include "ipc/shm.h"
#include "sync/spinlock.h"

static spinlock_t shm_lock = SPINLOCK_INIT;

shm_region_t shm_table[MAX_SHM_REGIONS];

void shm_init(void) {
    uint64_t flags;
    spin_lock_irqsave(&shm_lock, &flags);
    for (int i = 0; i < MAX_SHM_REGIONS; i++)
        shm_table[i].key = -1;
    spin_unlock_irqrestore(&shm_lock, flags);
}

void shm_lock_acquire(uint64_t *flags) {
    spin_lock_irqsave(&shm_lock, flags);
}

void shm_unlock_release(uint64_t flags) {
    spin_unlock_irqrestore(&shm_lock, flags);
}
