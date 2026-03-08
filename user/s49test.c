/*
 * s49test.c — Stage 49 Tests: Futex + Sleeping Primitives
 *
 * Phase 1: Futex syscall primitives
 * Phase 2: Userspace sleeping mutex (umutex)
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

/* ============================================================
 * Phase 1: Futex Primitives
 * ============================================================ */

static void test_futex_wait_wake(void) {
    /* Parent waits on futex in shared memory, child wakes it after a delay */
    long shm_id = sys_shmget(100, 1);
    volatile unsigned int *futex_val = (volatile unsigned int *)sys_shmat(shm_id);
    *futex_val = 1;

    long child_pid = sys_fork();
    if (child_pid == 0) {
        volatile unsigned int *cfv = (volatile unsigned int *)sys_shmat(shm_id);
        /* Yield a bit, then change value and wake parent */
        for (int i = 0; i < 20; i++)
            sys_yield();
        *cfv = 0;
        long r = sys_futex_wake(cfv, 1);
        sys_exit((int)r);  /* exit with number woken */
    }

    /* Parent: wait on futex (blocks because val == 1 == expected) */
    long r = sys_futex_wait(futex_val, 1);

    long child_status = sys_waitpid(child_pid);

    /* Test 1: futex_wait returned successfully (woken by child) */
    check(r == 0, "futex_wait blocks and is woken by futex_wake");

    /* Test 2: child's futex_wake returned 1 (woke 1 thread) */
    check(child_status == 1, "futex_wake returns number of threads woken");

    sys_shmdt((long)futex_val);
}

static void test_futex_wait_mismatch(void) {
    /* futex_wait should return immediately if value doesn't match */
    volatile unsigned int futex_val = 42;
    long r = sys_futex_wait((volatile unsigned int *)&futex_val, 99);  /* 42 != 99 */

    /* Test 3: returns -EAGAIN when value doesn't match */
    check(r == -11, "futex_wait returns EAGAIN on value mismatch");
}

static void test_futex_wake_nobody(void) {
    /* futex_wake on address with no waiters returns 0 */
    volatile unsigned int futex_val = 0;
    long r = sys_futex_wake((volatile unsigned int *)&futex_val, 10);

    /* Test 4: returns 0 when nobody is waiting */
    check(r == 0, "futex_wake returns 0 when no waiters");
}

static void test_futex_wake_multiple(void) {
    /* Fork 3 children, all wait on same futex in shared memory, parent wakes all.
     * Children use the inherited mapping (same vaddr as parent) since fork
     * gives COW copies but shm pages are shared. */
    long shm_id = sys_shmget(101, 1);
    volatile unsigned int *futex_val = (volatile unsigned int *)sys_shmat(shm_id);
    *futex_val = 1;
    long pids[3];

    for (int i = 0; i < 3; i++) {
        pids[i] = sys_fork();
        if (pids[i] == 0) {
            /* Re-attach shm to get a non-COW mapping to the shared page */
            volatile unsigned int *cfv = (volatile unsigned int *)sys_shmat(shm_id);
            sys_futex_wait(cfv, 1);
            sys_exit(0);
        }
    }

    /* Re-attach shm in parent to get non-COW mapping after fork */
    sys_shmdt((long)futex_val);
    futex_val = (volatile unsigned int *)sys_shmat(shm_id);

    /* Let children block */
    for (int i = 0; i < 100; i++)
        sys_yield();

    /* Wake all */
    *futex_val = 0;
    long woken = sys_futex_wake(futex_val, 10);

    /* Reap children */
    int all_ok = 1;
    for (int i = 0; i < 3; i++) {
        long st = sys_waitpid(pids[i]);
        if (st != 0) all_ok = 0;
    }

    /* Test 5: woke all 3 children */
    check(woken == 3, "futex_wake wakes multiple waiters");

    /* Test 6: all children exited cleanly */
    check(all_ok, "all woken children exit successfully");

    sys_shmdt((long)futex_val);
}

/* ============================================================
 * Phase 2: Userspace Sleeping Mutex
 * ============================================================ */

static void test_umutex_basic(void) {
    /* Basic lock/unlock */
    umutex_t m = UMUTEX_INIT;

    umutex_lock(&m);
    int locked = (m.state != 0);
    umutex_unlock(&m);
    int unlocked = (m.state == 0);

    /* Test 7: lock sets state, unlock clears it */
    check(locked && unlocked, "umutex lock/unlock basic");
}

static void test_umutex_trylock(void) {
    umutex_t m = UMUTEX_INIT;

    int r1 = umutex_trylock(&m);  /* should succeed */
    int r2 = umutex_trylock(&m);  /* should fail (already locked) */
    umutex_unlock(&m);
    int r3 = umutex_trylock(&m);  /* should succeed again */
    umutex_unlock(&m);

    /* Test 8: trylock semantics */
    check(r1 == 0 && r2 == -1 && r3 == 0, "umutex trylock succeeds/fails correctly");
}

static void test_umutex_contention(void) {
    /* Shared counter protected by umutex across parent and child.
     * Use shared memory so both processes see the same mutex + counter. */
    long shm_id = sys_shmget(42, 1);  /* 1 page */
    volatile unsigned char *shm = (volatile unsigned char *)sys_shmat(shm_id);
    if (!shm) {
        check(0, "umutex contention: shmget failed");
        return;
    }

    /* Layout: [umutex_t (4 bytes)] [counter (4 bytes)] */
    volatile unsigned int *mutex_state = (volatile unsigned int *)shm;
    volatile unsigned int *counter = (volatile unsigned int *)(shm + 64);
    *mutex_state = 0;  /* UMUTEX_INIT */
    *counter = 0;

    umutex_t *m = (umutex_t *)mutex_state;

    long child_pid = sys_fork();
    if (child_pid == 0) {
        /* Child: increment counter 500 times under lock */
        volatile unsigned char *cshm = (volatile unsigned char *)sys_shmat(shm_id);
        umutex_t *cm = (umutex_t *)cshm;
        volatile unsigned int *ccounter = (volatile unsigned int *)(cshm + 64);
        for (int i = 0; i < 500; i++) {
            umutex_lock(cm);
            unsigned int v = *ccounter;
            /* Yield in the middle to increase chance of race */
            if (i % 50 == 0) sys_yield();
            *ccounter = v + 1;
            umutex_unlock(cm);
        }
        sys_exit(0);
    }

    /* Parent: increment counter 500 times under lock */
    for (int i = 0; i < 500; i++) {
        umutex_lock(m);
        unsigned int v = *counter;
        if (i % 50 == 0) sys_yield();
        *counter = v + 1;
        umutex_unlock(m);
    }

    long st = sys_waitpid(child_pid);

    unsigned int final_count = *counter;

    /* Test 9: child exited ok */
    check(st == 0, "umutex contention: child exited cleanly");

    /* Test 10: counter is exactly 1000 (no lost increments) */
    check(final_count == 1000, "umutex contention: no lost increments (1000)");

    sys_shmdt((long)shm);
}

int main(void) {
    printf("=== Stage 49 Tests: Futex + Sleeping Primitives ===\n\n");

    printf("--- Phase 1: Futex Primitives ---\n");
    test_futex_wait_wake();
    test_futex_wait_mismatch();
    test_futex_wake_nobody();
    test_futex_wake_multiple();

    printf("\n--- Phase 2: Userspace Sleeping Mutex ---\n");
    test_umutex_basic();
    test_umutex_trylock();
    test_umutex_contention();

    printf("\n=== Stage 49 Results: %d/%d passed ===\n", pass_count, test_num);
    return 0;
}
