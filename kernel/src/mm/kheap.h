#ifndef LIMNX_KHEAP_H
#define LIMNX_KHEAP_H

#include <stdint.h>
#include <stddef.h>
#include "compiler.h"

#if defined(__x86_64__)
#define KHEAP_START  0xFFFFFFFF90000000ULL
#elif defined(__aarch64__)
/* ARM64 identity-mapped: heap in physical RAM above kernel */
#define KHEAP_START  0x0000000044000000ULL
#endif
#define KHEAP_MAX    (1024ULL * 1024 * 1024)  /* 1 GB */

void  kheap_init(void);
__must_check void *kmalloc(uint64_t size);
void  kfree(void *ptr);
__must_check void *krealloc(void *ptr, uint64_t new_size);

#endif
