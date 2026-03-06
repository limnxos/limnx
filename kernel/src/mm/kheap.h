#ifndef LIMNX_KHEAP_H
#define LIMNX_KHEAP_H

#include <stdint.h>
#include <stddef.h>

#define KHEAP_START  0xFFFFFFFF90000000ULL
#define KHEAP_MAX    (1024ULL * 1024 * 1024)  /* 1 GB */

void  kheap_init(void);
void *kmalloc(uint64_t size);
void  kfree(void *ptr);
void *krealloc(void *ptr, uint64_t new_size);

#endif
