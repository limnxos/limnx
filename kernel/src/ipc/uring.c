#define pr_fmt(fmt) "[uring] " fmt
#include "klog.h"

#include "ipc/uring.h"
#include "syscall/syscall.h"
#include "sync/spinlock.h"

static uring_instance_t uring_table[MAX_URING_INSTANCES];

/* Lock order: tier 5 (subsystem). See klog.h for full hierarchy */
static spinlock_t uring_lock = SPINLOCK_INIT;

int uring_create(uint32_t entries) {
    if (entries == 0 || entries > URING_MAX_ENTRIES)
        entries = URING_MAX_ENTRIES;

    uint64_t flags;
    spin_lock_irqsave(&uring_lock, &flags);

    for (int i = 0; i < MAX_URING_INSTANCES; i++) {
        if (!uring_table[i].used) {
            uring_table[i].used = 1;
            uring_table[i].refs = 1;
            uring_table[i].max_entries = entries;
            spin_unlock_irqrestore(&uring_lock, flags);
            return i;
        }
    }

    spin_unlock_irqrestore(&uring_lock, flags);
    pr_err("uring table full\n");
    return -1;
}

void uring_close(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return;

    uint64_t flags;
    spin_lock_irqsave(&uring_lock, &flags);

    uring_instance_t *ur = &uring_table[idx];
    if (!ur->used) {
        spin_unlock_irqrestore(&uring_lock, flags);
        return;
    }
    if (ur->refs > 0) ur->refs--;
    if (ur->refs == 0)
        ur->used = 0;

    spin_unlock_irqrestore(&uring_lock, flags);
}

void uring_ref(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return;

    uint64_t flags;
    spin_lock_irqsave(&uring_lock, &flags);
    if (uring_table[idx].used)
        uring_table[idx].refs++;
    spin_unlock_irqrestore(&uring_lock, flags);
}

uring_instance_t *uring_get(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return NULL;
    if (!uring_table[idx].used) return NULL;
    return &uring_table[idx];
}

int uring_index(uring_instance_t *ur) {
    if (!ur) return -1;
    for (int i = 0; i < MAX_URING_INSTANCES; i++) {
        if (&uring_table[i] == ur)
            return i;
    }
    return -1;
}
