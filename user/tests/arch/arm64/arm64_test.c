/*
 * arm64_test.c — ARM64-specific tests
 * Tests: signal frame (34-reg save/restore), fork COW, SVC dispatch
 * ARM64 ONLY — do not build for x86_64.
 */
#include "../../limntest.h"

static volatile int arm64_sig_ok = 0;

static void arm64_sig_handler(int sig) {
    (void)sig;
    arm64_sig_ok = 1;
    sys_sigreturn();
}

static void test_signal_across_fork(void) {
    /* ARM64 signal delivery requires per-thread exception frame + 34-reg save.
     * This was the Stage 106 3-bug fix. Verify it still works. */
    arm64_sig_ok = 0;
    sys_sigaction(10, arm64_sig_handler);

    long parent = sys_getpid();
    long child = sys_fork();
    if (child == 0) {
        long ts[2] = {0, 50000000};
        sys_nanosleep(ts);
        sys_kill(parent, 10);
        sys_exit(0);
    }
    long ts[2] = {0, 50000000};
    for (int i = 0; i < 20 && !arm64_sig_ok; i++)
        sys_nanosleep(ts);
    lt_ok(arm64_sig_ok == 1, "signal delivered across fork (34-reg frame)");
    sys_waitpid(child);
}

static void test_cow_fork(void) {
    /* ARM64 COW requires PTE_MAKE_READONLY/WRITABLE HAL macros */
    volatile int *val = (volatile int *)malloc(sizeof(int));
    if (!val) { lt_ok(0, "COW setup"); return; }
    *val = 42;
    long child = sys_fork();
    if (child == 0) {
        *val = 99;
        sys_exit(*val == 99 ? 0 : 1);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "child COW write works");
    lt_ok(*val == 42, "parent untouched after child COW");
    free((void *)val);
}

static void test_svc_dispatch(void) {
    /* Basic SVC → kernel → return path */
    long pid = sys_getpid();
    lt_ok(pid > 0, "SVC dispatch works (getpid)");
    sys_yield();
    lt_ok(1, "yield returns (SVC round-trip)");
}

int main(void) {
    lt_suite("arch/arm64");
    test_signal_across_fork();
    test_cow_fork();
    test_svc_dispatch();
    return lt_done();
}
