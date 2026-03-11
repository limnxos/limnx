/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIMNX_KUTIL_H
#define LIMNX_KUTIL_H

/*
 * Shared kernel string and memory utilities.
 * Modeled after Linux include/linux/string.h
 *
 * All functions are static inline to avoid linker issues
 * across multiple translation units.
 */

#include <stdint.h>

/**
 * str_eq - Compare two null-terminated strings for equality.
 * Returns 1 if equal, 0 otherwise.
 */
static inline int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

/**
 * str_copy - Copy a string with bounds checking.
 * @dst: destination buffer
 * @src: source string
 * @max: size of destination buffer (includes null terminator)
 *
 * Always null-terminates. Copies at most max-1 characters.
 */
static inline void str_copy(char *dst, const char *src, uint64_t max) {
    uint64_t i = 0;
    if (max == 0) return;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/**
 * str_len - Return the length of a null-terminated string.
 */
static inline uint64_t str_len(const char *s) {
    uint64_t len = 0;
    while (s[len]) len++;
    return len;
}

/**
 * mem_copy - Copy n bytes from src to dst.
 * Does not handle overlapping regions.
 */
static inline void mem_copy(void *dst, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];
}

/**
 * mem_set - Fill n bytes of dst with value c.
 */
static inline void mem_set(void *dst, uint8_t c, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++)
        d[i] = c;
}

/**
 * mem_zero - Zero n bytes at dst.
 */
static inline void mem_zero(void *dst, uint64_t n) {
    mem_set(dst, 0, n);
}

#endif
