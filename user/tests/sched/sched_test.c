/*
 * sched_test.c — Scheduler tests
 * Tests: concurrent fork, yield, nanosleep
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static void test_yield(void) {
    /* Yield should return without error */
    sys_yield();
    sys_yield();
    sys_yield();
    lt_ok(1, "yield returns 3 times");
}

static void test_concurrent_fork_exit(void) {
    int n = 8;
    long pids[8];
    for (int i = 0; i < n; i++) {
        pids[i] = sys_fork();
        if (pids[i] == 0) {
            /* Child: do some work then exit */
            for (int j = 0; j < 10; j++) sys_yield();
            sys_exit(i);
        }
    }
    int all_ok = 1;
    for (int i = 0; i < n; i++) {
        long st = sys_waitpid(pids[i]);
        if (st != i) all_ok = 0;
    }
    lt_ok(all_ok, "8 concurrent fork+exit with correct status");
}

static void test_fork_yield_interleave(void) {
    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 20; i++) sys_yield();
        sys_exit(0);
    }
    for (int i = 0; i < 20; i++) sys_yield();
    long st = sys_waitpid(child);
    lt_ok(st == 0, "parent+child yield interleave");
}

static void test_nanosleep_basic(void) {
    long ts[2] = {0, 20000000};  /* 20ms */
    long ret = sys_nanosleep(ts);
    lt_ok(ret == 0, "nanosleep 20ms returns");
}

int main(void) {
    lt_suite("sched");
    test_yield();
    test_concurrent_fork_exit();
    test_fork_yield_interleave();
    test_nanosleep_basic();
    return lt_done();
}
