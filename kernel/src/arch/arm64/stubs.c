/*
 * ARM64 stub functions for subsystems not yet ported.
 * These provide no-op or error-returning implementations so shared
 * kernel code can link without #ifdef pollution.
 *
 * As real drivers are added (virtio-mmio blk/net, etc.), stubs are
 * removed and replaced by the actual implementation.
 */

#include <stdint.h>
#include "errno.h"

/* --- Scheduler smoke test stub --- */

void sched_smoke_test(void) { }
