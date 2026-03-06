#ifndef LIMNX_BCACHE_H
#define LIMNX_BCACHE_H

#include <stdint.h>

#define BCACHE_ENTRIES 256
#define BLOCK_SIZE     4096

void     bcache_init(void);
int      bcache_read(uint32_t block_no, void *buf);
int      bcache_write(uint32_t block_no, const void *buf);
void     bcache_invalidate(uint32_t block_no);
void     bcache_flush(void);
uint32_t bcache_dirty_count(void);
void     bcache_stats(uint64_t *hits, uint64_t *misses);

#endif
