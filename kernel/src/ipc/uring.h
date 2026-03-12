#ifndef LIMNX_URING_H
#define LIMNX_URING_H

#include <stdint.h>

#define MAX_URING_INSTANCES  4
#define URING_MAX_ENTRIES    32

typedef struct uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t reserved;
    int32_t  fd;
    uint64_t off;
    uint64_t addr;
    uint32_t len;
    uint32_t user_data;
} __attribute__((packed)) uring_sqe_t;

typedef struct uring_cqe {
    uint32_t user_data;
    int32_t  res;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed)) uring_cqe_t;

typedef struct uring_instance {
    uint8_t  used;
    uint32_t refs;
    uint32_t max_entries;
} uring_instance_t;

/* Allocate a new uring instance, returns index or -1 */
int uring_create(uint32_t entries);

/* Close instance (decrements refs) */
void uring_close(int idx);

/* Increment ref count */
void uring_ref(int idx);

/* Get instance by index */
uring_instance_t *uring_get(int idx);

/* Find index of an instance pointer, returns -1 if not found */
int uring_index(const uring_instance_t *ur);

#endif
