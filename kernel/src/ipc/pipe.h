#ifndef LIMNX_IPC_PIPE_H
#define LIMNX_IPC_PIPE_H

#include <stdint.h>

#define PIPE_BUF_SIZE 4096
#define MAX_PIPES     8

typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    uint8_t  closed_read;
    uint8_t  closed_write;
    uint8_t  used;
    uint32_t read_refs;
    uint32_t write_refs;
} pipe_t;

extern pipe_t pipes[MAX_PIPES];

/* Lock/unlock the global pipe table */
void pipe_lock_acquire(uint64_t *flags);
void pipe_unlock_release(uint64_t flags);

#endif
