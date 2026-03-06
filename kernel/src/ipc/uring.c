#include "ipc/uring.h"
#include "syscall/syscall.h"

static uring_instance_t uring_table[MAX_URING_INSTANCES];

int uring_create(uint32_t entries) {
    if (entries == 0 || entries > URING_MAX_ENTRIES)
        entries = URING_MAX_ENTRIES;

    for (int i = 0; i < MAX_URING_INSTANCES; i++) {
        if (!uring_table[i].used) {
            uring_table[i].used = 1;
            uring_table[i].refs = 1;
            uring_table[i].max_entries = entries;
            return i;
        }
    }
    return -1;
}

void uring_close(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return;
    uring_instance_t *ur = &uring_table[idx];
    if (!ur->used) return;
    if (ur->refs > 0) ur->refs--;
    if (ur->refs == 0)
        ur->used = 0;
}

void uring_ref(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return;
    if (uring_table[idx].used)
        uring_table[idx].refs++;
}

uring_instance_t *uring_get(int idx) {
    if (idx < 0 || idx >= MAX_URING_INSTANCES) return (void *)0;
    if (!uring_table[idx].used) return (void *)0;
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
