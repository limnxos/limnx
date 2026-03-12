#ifndef LIMNX_ARCH_PTE_H
#define LIMNX_ARCH_PTE_H

/*
 * Architecture-specific page table entry bit definitions.
 * Dispatches to the correct arch header.
 */

#if defined(__x86_64__)
#include "arch/x86_64/pte.h"
#elif defined(__aarch64__)
#include "arch/arm64/pte.h"
#else
#error "Unsupported architecture"
#endif

#endif
