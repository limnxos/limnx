/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIMNX_COMPILER_H
#define LIMNX_COMPILER_H

/*
 * Compiler attributes and helpers.
 * Modeled after Linux include/linux/compiler_attributes.h
 */

/* Mark functions whose return value must be checked */
#define __must_check    __attribute__((warn_unused_result))

/* Mark intentionally unused variables/parameters */
#define __unused        __attribute__((unused))

/* Struct packing for hardware/disk layouts */
#define __packed        __attribute__((packed))

/* Explicit alignment */
#define __aligned(x)    __attribute__((aligned(x)))

/* Branch prediction hints */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

/*
 * User-space pointer annotation.
 * No enforcement (we don't run sparse), but documents that
 * a pointer originates from user space and must be validated
 * before dereference.
 */
#define __user

/* NULL pointer — matches stddef.h */
#ifndef NULL
#define NULL ((void *)0)
#endif

#endif
