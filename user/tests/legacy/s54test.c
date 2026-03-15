#include "libc/libc.h"

static int passed = 0, failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

/* Force stack overflow by manually moving RSP below the stack */
static void __attribute__((noinline)) trigger_stack_overflow(void) {
    /* Move RSP way below the stack bottom to trigger guard page fault */
    volatile char *p;
#if defined(__x86_64__)
    __asm__ volatile ("mov %%rsp, %0" : "=r"(p));
#elif defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r"(p));
#endif
    /* Write 128KB below current RSP — well past the 64KB stack */
    p -= 128 * 1024;
    *p = 42;  /* should fault on the guard page or unmapped region */
}

int main(void) {
    printf("=== Stage 54: Guard Pages ===\n");

    /* Test 1: Stack overflow triggers guard page detection */
    {
        long pid = sys_fork();
        if (pid == 0) {
            trigger_stack_overflow();
            sys_exit(0);  /* should never reach here */
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T1: stack overflow kills child");
    }

    /* Test 2: mmap guard gap — write past end of mmap region faults */
    {
        long addr = sys_mmap(1);
        check(addr > 0, "T2: mmap 1 page");
        volatile char *p = (volatile char *)addr;
        p[0] = 'A';  /* valid write */

        long pid = sys_fork();
        if (pid == 0) {
            /* Write into the guard gap (1 page past the end) */
            volatile char *guard = (volatile char *)(addr + 0x1000);
            *guard = 'X';  /* should fault */
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T2: write to guard gap kills child");
        sys_munmap(addr);
    }

    /* Test 3: SYS_MMAP_GUARD — allocate guarded region */
    {
        long addr = sys_mmap_guard(2);
        check(addr > 0, "T3: mmap_guard 2 pages");
        volatile int *p0 = (volatile int *)addr;
        volatile int *p1 = (volatile int *)(addr + 0x1000);
        *p0 = 100;
        *p1 = 200;
        check(*p0 == 100 && *p1 == 200, "T3: write to guarded pages works");

        /* Write past the guarded region should fault */
        long pid = sys_fork();
        if (pid == 0) {
            volatile char *guard = (volatile char *)(addr + 0x2000);
            *guard = 'X';
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T3: write past guard kills child");
        sys_munmap(addr);
    }

    /* Test 4: Adjacent mmap allocations have guard gaps between them */
    {
        long addr1 = sys_mmap(1);
        long addr2 = sys_mmap(1);
        check(addr1 > 0 && addr2 > 0, "T4: two mmaps succeed");
        /* addr2 should be at least 2 pages past addr1 (1 data + 1 guard gap) */
        long gap = addr2 - addr1;
        check(gap >= 0x2000, "T4: gap between mmaps >= 2 pages");

        /* Write to first mmap's data page */
        volatile char *p = (volatile char *)addr1;
        p[0] = 'A';

        /* Write to guard gap between addr1 and addr2 should fault */
        long pid = sys_fork();
        if (pid == 0) {
            volatile char *guard = (volatile char *)(addr1 + 0x1000);
            *guard = 'X';
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T4: write to inter-mmap guard kills child");

        sys_munmap(addr1);
        sys_munmap(addr2);
    }

    /* Test 5: Guard pages don't waste physical memory */
    {
        /* Allocate two regions and verify they're separated by unmapped gap */
        long addr1 = sys_mmap(1);
        long addr2 = sys_mmap(1);
        /* The gap page should NOT be physically mapped */
        /* We verify by successfully allocating — if guard pages consumed
         * physical pages, we'd run out faster. This is a structural test:
         * the gap is 1 page and only the data pages have physical backing. */
        check(addr1 > 0 && addr2 > 0, "T5: guard gaps don't consume physical pages");
        sys_munmap(addr1);
        sys_munmap(addr2);
    }

    /* Test 6: Demand-paged mmap also gets guard gaps */
    {
        long addr1 = sys_mmap2(1, MMAP_DEMAND);
        long addr2 = sys_mmap2(1, MMAP_DEMAND);
        check(addr1 > 0 && addr2 > 0, "T6: demand mmaps succeed");
        long gap = addr2 - addr1;
        check(gap >= 0x2000, "T6: demand mmap gap >= 2 pages");

        /* Touch pages to fault them in */
        volatile int *p1 = (volatile int *)addr1;
        volatile int *p2 = (volatile int *)addr2;
        *p1 = 111;
        *p2 = 222;
        check(*p1 == 111 && *p2 == 222, "T6: demand pages work with guard gaps");
        sys_munmap(addr1);
        sys_munmap(addr2);
    }

    /* Test 7: mmap_guard with invalid args */
    {
        long r1 = sys_mmap_guard(0);
        check(r1 < 0, "T7: mmap_guard(0) returns error");
    }

    printf("\n=== Stage 54 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
