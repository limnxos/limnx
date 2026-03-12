#ifndef LIMNX_IPC_SHM_H
#define LIMNX_IPC_SHM_H

#include <stdint.h>

#define MAX_SHM_REGIONS 16

typedef struct shm_region {
    int32_t  key;
    uint64_t phys_pages[16];
    uint32_t num_pages;
    uint32_t ref_count;
} shm_region_t;

extern shm_region_t shm_table[MAX_SHM_REGIONS];

void shm_init(void);
void shm_lock_acquire(uint64_t *flags);
void shm_unlock_release(uint64_t flags);

#endif
