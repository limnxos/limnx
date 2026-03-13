#ifndef LIMNX_ARCH_IO_H
#define LIMNX_ARCH_IO_H

#if defined(__x86_64__)
#include "arch/x86_64/io.h"
#elif defined(__aarch64__)
/* ARM64 has no port I/O; PCI uses MMIO (ECAM). Stubs return 0xFF. */
#include <stdint.h>
static inline void outb(uint16_t p, uint8_t v) { (void)p;(void)v; }
static inline uint8_t inb(uint16_t p) { (void)p; return 0xFF; }
static inline void outw(uint16_t p, uint16_t v) { (void)p;(void)v; }
static inline uint16_t inw(uint16_t p) { (void)p; return 0xFFFF; }
static inline void outl(uint16_t p, uint32_t v) { (void)p;(void)v; }
static inline uint32_t inl(uint16_t p) { (void)p; return 0xFFFFFFFF; }
static inline void io_wait(void) {}
#endif

#endif
