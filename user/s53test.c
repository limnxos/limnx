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

int main(void) {
    printf("=== Stage 53: mprotect ===\n");

    /* Test 1: Basic mprotect — mmap RW, write, mprotect RO, read still works */
    {
        long addr = sys_mmap(1);
        check(addr > 0, "T1: mmap 1 page");
        volatile int *p = (volatile int *)addr;
        *p = 0xDEADBEEF;
        long r = sys_mprotect(addr, 1, PROT_READ);
        check(r == 0, "T1: mprotect RO returns 0");
        int val = *p;
        check(val == (int)0xDEADBEEF, "T1: read after RO works");
        /* Restore so we can munmap cleanly */
        sys_mprotect(addr, 1, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    /* Test 2: Write after RO mprotect causes fault (fork child dies) */
    {
        long addr = sys_mmap(1);
        volatile int *p = (volatile int *)addr;
        *p = 42;
        sys_mprotect(addr, 1, PROT_READ);

        long pid = sys_fork();
        if (pid == 0) {
            /* Child: try to write — should fault and die */
            *p = 99;
            /* If we get here, the protection didn't work */
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        /* Child should have been killed by page fault (negative exit status) */
        check(status != 0, "T2: write to RO page kills child");
        sys_mprotect(addr, 1, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    /* Test 3: PROT_NONE — any access causes fault */
    {
        long addr = sys_mmap(1);
        volatile int *p = (volatile int *)addr;
        *p = 123;
        sys_mprotect(addr, 1, PROT_NONE);

        long pid = sys_fork();
        if (pid == 0) {
            /* Child: try to read — should fault */
            volatile int val = *p;
            (void)val;
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T3: read from PROT_NONE kills child");
        sys_mprotect(addr, 1, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    /* Test 4: Restore write — RW → RO → RW, write succeeds */
    {
        long addr = sys_mmap(1);
        volatile int *p = (volatile int *)addr;
        *p = 1;
        sys_mprotect(addr, 1, PROT_READ);
        sys_mprotect(addr, 1, PROT_READ | PROT_WRITE);
        *p = 2;
        check(*p == 2, "T4: write after RO→RW works");
        sys_munmap(addr);
    }

    /* Test 5: Partial range — 4 pages, protect middle 2 as RO */
    {
        long addr = sys_mmap(4);
        volatile int *p0 = (volatile int *)addr;
        volatile int *p1 = (volatile int *)(addr + 0x1000);
        volatile int *p2 = (volatile int *)(addr + 0x2000);
        volatile int *p3 = (volatile int *)(addr + 0x3000);
        *p0 = 10; *p1 = 20; *p2 = 30; *p3 = 40;

        /* Protect pages 1 and 2 as read-only */
        sys_mprotect(addr + 0x1000, 2, PROT_READ);

        /* Pages 0 and 3 should still be writable */
        *p0 = 11;
        *p3 = 41;
        check(*p0 == 11 && *p3 == 41, "T5: unprotected pages still writable");

        /* Fork child to test write to protected page */
        long pid = sys_fork();
        if (pid == 0) {
            *p1 = 99;  /* should fault */
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T5: write to partial-RO page kills child");

        /* Data in RO pages preserved */
        check(*p1 == 20 && *p2 == 30, "T5: RO page data preserved");

        sys_mprotect(addr + 0x1000, 2, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    /* Test 6: PROT_READ | PROT_EXEC — removes NX bit */
    {
        long addr = sys_mmap(1);
        long r = sys_mprotect(addr, 1, PROT_READ | PROT_EXEC);
        check(r == 0, "T6: mprotect R+X succeeds");
        /* We can't easily test execution, but verify the syscall returns 0 */
        sys_mprotect(addr, 1, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    /* Test 7: Invalid args */
    {
        long addr = sys_mmap(1);
        /* Unaligned address */
        long r1 = sys_mprotect(addr + 1, 1, PROT_READ);
        check(r1 < 0, "T7: unaligned addr returns error");
        /* Zero pages */
        long r2 = sys_mprotect(addr, 0, PROT_READ);
        check(r2 < 0, "T7: zero pages returns error");
        /* Address not in any mmap entry */
        long r3 = sys_mprotect(0x1000, 1, PROT_READ);
        check(r3 < 0, "T7: unmapped addr returns error");
        sys_munmap(addr);
    }

    /* Test 8: Demand-paged mmap + mprotect */
    {
        long addr = sys_mmap2(2, MMAP_DEMAND);
        check(addr > 0, "T8: demand mmap 2 pages");
        /* Touch pages to fault them in */
        volatile int *p0 = (volatile int *)addr;
        volatile int *p1 = (volatile int *)(addr + 0x1000);
        *p0 = 100;
        *p1 = 200;
        /* Now mprotect to read-only */
        long r = sys_mprotect(addr, 2, PROT_READ);
        check(r == 0, "T8: mprotect demand pages RO");
        check(*p0 == 100 && *p1 == 200, "T8: read after RO works");

        /* Fork child to verify write fault */
        long pid = sys_fork();
        if (pid == 0) {
            *p0 = 999;
            sys_exit(0);
        }
        long status = sys_waitpid(pid);
        check(status != 0, "T8: write to RO demand page kills child");
        sys_mprotect(addr, 2, PROT_READ | PROT_WRITE);
        sys_munmap(addr);
    }

    printf("\n=== Stage 53 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return (failed > 0) ? 1 : 0;
}
