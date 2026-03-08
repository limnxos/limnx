/*
 * s51test.c — Stage 51 Tests: COW Shared Memory Fix
 *
 * Tests that shared memory pages are NOT marked COW on fork,
 * so parent and child can read/write shm without re-attaching.
 */

#include "libc/libc.h"

static int test_num = 0;
static int pass_count = 0;

static void check(int ok, const char *desc) {
    test_num++;
    if (ok) {
        printf("  [%d] PASS: %s\n", test_num, desc);
        pass_count++;
    } else {
        printf("  [%d] FAIL: %s\n", test_num, desc);
    }
}

static void test_shm_write_after_fork(void) {
    /* Parent creates shm, writes value, forks.
     * Child writes different value to shm.
     * Parent reads shm after child exits — should see child's value. */
    long shmid = sys_shmget(100, 1);
    check(shmid >= 0, "shmget succeeds");

    long addr = sys_shmat(shmid);
    check(addr > 0, "shmat succeeds");

    volatile int *shared = (volatile int *)addr;
    *shared = 42;

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: write to shm directly (no re-attach needed) */
        *shared = 99;
        sys_exit(0);
    }

    /* Parent: wait for child, then check */
    sys_waitpid(pid);
    check(*shared == 99, "parent sees child's shm write after fork");

    sys_shmdt(addr);
}

static void test_shm_bidirectional(void) {
    /* Both parent and child write to different offsets in shm */
    long shmid = sys_shmget(101, 1);
    long addr = sys_shmat(shmid);
    if (addr <= 0) {
        check(0, "shm bidirectional: shmat failed");
        return;
    }

    volatile int *buf = (volatile int *)addr;
    buf[0] = 0;  /* parent slot */
    buf[1] = 0;  /* child slot */

    long pid = sys_fork();
    if (pid == 0) {
        /* Child writes to slot 1 */
        buf[1] = 0xBEEF;
        /* Spin until parent writes slot 0 */
        for (int i = 0; i < 100000; i++) {
            if (buf[0] == 0xCAFE) break;
            sys_yield();
        }
        sys_exit(0);
    }

    /* Parent writes to slot 0 */
    buf[0] = 0xCAFE;
    sys_waitpid(pid);

    check(buf[1] == 0xBEEF, "parent sees child write in shm slot");
    check(buf[0] == 0xCAFE, "parent's own shm write persists");

    sys_shmdt(addr);
}

static void test_futex_on_shm_after_fork(void) {
    /* Futex on shared memory should work without re-attach */
    long shmid = sys_shmget(102, 1);
    long addr = sys_shmat(shmid);
    if (addr <= 0) {
        check(0, "futex shm: shmat failed");
        return;
    }

    volatile int *futex_val = (volatile int *)addr;
    *futex_val = 0;

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: wait a bit, then set value and wake parent */
        for (int i = 0; i < 50; i++) sys_yield();
        *futex_val = 1;
        sys_futex_wake((int *)futex_val, 1);
        sys_exit(0);
    }

    /* Parent: futex_wait until child wakes us */
    int waited = 0;
    for (int i = 0; i < 200; i++) {
        if (*futex_val == 1) { waited = 1; break; }
        sys_futex_wait((int *)futex_val, 0);
        waited = 1;
    }
    sys_waitpid(pid);

    check(*futex_val == 1 && waited, "futex on shm works after fork without re-attach");

    sys_shmdt(addr);
}

static void test_private_pages_still_cow(void) {
    /* Ensure regular (non-shm) pages are still COW after fork */
    volatile int local_val = 42;

    long pid = sys_fork();
    if (pid == 0) {
        /* Child modifies local variable (should be COW copy) */
        local_val = 99;
        /* Parent should NOT see this change */
        sys_exit(0);
    }

    sys_waitpid(pid);
    check(local_val == 42, "private stack pages still COW after fork");
}

static void test_multi_shm_regions(void) {
    /* Multiple shm regions should all survive fork without COW */
    long shmid1 = sys_shmget(103, 1);
    long shmid2 = sys_shmget(104, 1);
    long addr1 = sys_shmat(shmid1);
    long addr2 = sys_shmat(shmid2);

    if (addr1 <= 0 || addr2 <= 0) {
        check(0, "multi shm: shmat failed");
        return;
    }

    volatile int *s1 = (volatile int *)addr1;
    volatile int *s2 = (volatile int *)addr2;
    *s1 = 10;
    *s2 = 20;

    long pid = sys_fork();
    if (pid == 0) {
        *s1 = 11;
        *s2 = 22;
        sys_exit(0);
    }

    sys_waitpid(pid);
    check(*s1 == 11, "first shm region writable after fork");
    check(*s2 == 22, "second shm region writable after fork");

    sys_shmdt(addr1);
    sys_shmdt(addr2);
}

int main(void) {
    printf("=== Stage 51 Tests: COW Shared Memory Fix ===\n\n");

    test_shm_write_after_fork();
    test_shm_bidirectional();
    test_futex_on_shm_after_fork();
    test_private_pages_still_cow();
    test_multi_shm_regions();

    printf("\n=== Stage 51 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
