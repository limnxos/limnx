/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LIMNX_KLOG_H
#define LIMNX_KLOG_H

/*
 * Kernel logging framework.
 * Modeled after Linux printk / pr_* macros.
 *
 * Usage: Each .c file defines pr_fmt before including this header:
 *
 *   #define pr_fmt(fmt) "[pmm] " fmt
 *   #include "klog.h"
 *
 *   pr_info("initialized %lu pages\n", count);
 *   // Output: [pmm] initialized 4096 pages
 *
 *   pr_err("allocation failed\n");
 *   // Output: [pmm] ERROR: allocation failed
 */

#include "serial.h"
#include "compiler.h"
#include "arch/cpu.h"

/* Log levels (matching Linux KERN_* severity) */
#define KLOG_EMERG   0  /* System is unusable — followed by panic */
#define KLOG_ERR     1  /* Error conditions */
#define KLOG_WARN    2  /* Warning conditions */
#define KLOG_INFO    3  /* Informational */
#define KLOG_DEBUG   4  /* Debug-level (compiled out by default) */

/* Compile-time log level filter — override with -DKLOG_LEVEL=N */
#ifndef KLOG_LEVEL
#define KLOG_LEVEL KLOG_INFO
#endif

/* Default pr_fmt if not defined by the including file */
#ifndef pr_fmt
#define pr_fmt(fmt) fmt
#endif

/* Core logging macro */
#define klog(level, fmt, ...) do { \
    if ((level) <= KLOG_LEVEL) \
        serial_printf(pr_fmt(fmt), ##__VA_ARGS__); \
} while (0)

/* Convenience macros — equivalent to Linux pr_err, pr_warn, pr_info */
#define pr_emerg(fmt, ...) klog(KLOG_EMERG, fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)   klog(KLOG_ERR,   "ERROR: " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  klog(KLOG_WARN,  "WARN: "  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)  klog(KLOG_INFO,  fmt, ##__VA_ARGS__)

/* pr_debug — compiled out unless DEBUG is defined */
#ifdef DEBUG
#define pr_debug(fmt, ...) klog(KLOG_DEBUG, "DEBUG: " fmt, ##__VA_ARGS__)
#else
#define pr_debug(fmt, ...) do { (void)0; } while (0)
#endif

/*
 * panic() — halt the system on unrecoverable error.
 * Equivalent to Linux kernel panic().
 * Use for: PMM init failure, heap init failure, scheduler init failure.
 * Do NOT use for recoverable errors — use pr_err + error return instead.
 */
#define panic(fmt, ...) do { \
    serial_printf("KERNEL PANIC: " fmt "\n", ##__VA_ARGS__); \
    serial_printf("  at %s:%d\n", __FILE__, __LINE__); \
    arch_irq_disable(); \
    for (;;) arch_halt(); \
} while (0)

/*
 * BUG_ON(condition) — halt if invariant is violated.
 * Equivalent to Linux BUG_ON().
 * Use for: conditions that should NEVER be true and indicate
 * a kernel programming error (e.g., NULL current_thread in scheduler).
 */
#define BUG_ON(cond) do { \
    if (unlikely(!!(cond))) \
        panic("BUG: %s", #cond); \
} while (0)

/*
 * WARN_ON(condition) — log warning but continue.
 * Equivalent to Linux WARN_ON().
 * Use for: unexpected but recoverable conditions that indicate
 * a possible bug but the system can continue safely.
 */
#define WARN_ON(cond) do { \
    if (unlikely(!!(cond))) \
        serial_printf("WARNING: (%s) at %s:%d\n", \
                       #cond, __FILE__, __LINE__); \
} while (0)

/*
 * WARN_ON_ONCE(condition) — log warning only the first time.
 * Prevents log flooding from repeated triggering.
 */
#define WARN_ON_ONCE(cond) do { \
    static int __warned = 0; \
    if (unlikely(!!(cond)) && !__warned) { \
        __warned = 1; \
        serial_printf("WARNING: (%s) at %s:%d\n", \
                       #cond, __FILE__, __LINE__); \
    } \
} while (0)

#endif
