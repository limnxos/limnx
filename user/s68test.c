/* Stage 68 tests: SMP race fix — waitpid sleep/wake + SIGCHLD filtering */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

static volatile int sigchld_count = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    sigchld_count++;
    sys_sigreturn();
}

static volatile int got_sigusr = 0;
static void sigusr_handler(int sig) {
    (void)sig;
    got_sigusr = 1;
    sys_sigreturn();
}

/* --- Test 1: basic waitpid blocks and returns correct status --- */
static void test_waitpid_basic(void) {
    long child = sys_fork();
    if (child == 0) {
        sys_exit(42);
    }
    long st = sys_waitpid(child);
    TEST("waitpid returns correct exit status (42)", st == 42);
}

/* --- Test 2: waitpid with SIGCHLD handler doesn't return -EINTR --- */
static void test_waitpid_sigchld_no_eintr(void) {
    sys_sigaction3(SIGCHLD, sigchld_handler, 0);
    sigchld_count = 0;

    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 10; i++) sys_yield();
        sys_exit(77);
    }

    /* waitpid should NOT return -EINTR from SIGCHLD */
    long st = sys_waitpid(child);
    TEST("waitpid with SIGCHLD handler returns status (77)", st == 77);
    /* SIGCHLD may or may not have been delivered yet depending on timing */

    /* Reset handler */
    sys_sigaction(SIGCHLD, (void (*)(int))0);
}

/* --- Test 3: waitpid returns -EINTR for non-SIGCHLD signals --- */
static void test_waitpid_other_signal_eintr(void) {
    sys_sigaction3(SIGINT, sigusr_handler, 0);
    got_sigusr = 0;

    long ppid = sys_getpid();

    long child = sys_fork();
    if (child == 0) {
        /* Long-running child */
        for (int i = 0; i < 50; i++) sys_yield();
        sys_exit(99);
    }

    /* Signaler sends SIGINT while parent is in waitpid */
    long signaler = sys_fork();
    if (signaler == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    long st = sys_waitpid(child);
    TEST("waitpid returns -EINTR for SIGINT", st == -EINTR);
    TEST("SIGINT handler was called", got_sigusr == 1);

    /* Retry to reap child */
    long st2;
    while ((st2 = sys_waitpid(child)) == -EINTR)
        ;
    /* Child may have exited with 99 or been reaped already (-1) */
    TEST("retry waitpid gets child status", st2 == 99 || st2 == -1);

    sys_waitpid(signaler);
}

/* --- Test 4: rapid fork+waitpid (stress test for sleep/wake) --- */
static void test_rapid_fork_wait(void) {
    int ok = 1;
    for (int i = 0; i < 10; i++) {
        long child = sys_fork();
        if (child == 0) {
            sys_exit(i + 1);
        }
        long st = sys_waitpid(child);
        if (st != i + 1) ok = 0;
    }
    TEST("10 rapid fork+waitpid all correct", ok);
}

/* --- Test 5: multiple children, wait for each --- */
static void test_multi_child_wait(void) {
    long pids[5];
    for (int i = 0; i < 5; i++) {
        pids[i] = sys_fork();
        if (pids[i] == 0) {
            /* Each child yields different amounts to create timing variety */
            for (int j = 0; j < (i + 1) * 3; j++) sys_yield();
            sys_exit(10 + i);
        }
    }

    int ok = 1;
    for (int i = 0; i < 5; i++) {
        long st = sys_waitpid(pids[i]);
        if (st != 10 + i) ok = 0;
    }
    TEST("5 children waited correctly", ok);
}

/* --- Test 6: WNOHANG still works --- */
static void test_wnohang(void) {
    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 20; i++) sys_yield();
        sys_exit(88);
    }

    /* Immediate WNOHANG should return 0 (not exited yet) */
    long st = sys_waitpid_flags(child, 1 /* WNOHANG */);
    TEST("WNOHANG returns 0 for running child", st == 0);

    /* Now blocking wait */
    st = sys_waitpid(child);
    TEST("blocking waitpid after WNOHANG returns status (88)", st == 88);
}

/* --- Test 7: SA_RESTART with waitpid and non-SIGCHLD signal --- */
static void test_waitpid_sa_restart(void) {
    sys_sigaction3(SIGINT, sigusr_handler, SA_RESTART);
    got_sigusr = 0;

    long ppid = sys_getpid();

    long child = sys_fork();
    if (child == 0) {
        for (int i = 0; i < 30; i++) sys_yield();
        sys_exit(66);
    }

    long signaler = sys_fork();
    if (signaler == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(ppid, SIGINT);
        sys_exit(0);
    }

    /* With SA_RESTART, waitpid should auto-restart and return real status */
    long st = sys_waitpid(child);
    TEST("SA_RESTART waitpid gets exit status (66)", st == 66);
    TEST("SA_RESTART: signal handler was called", got_sigusr == 1);

    sys_waitpid(signaler);
}

/* --- Test 8: child exits before parent calls waitpid --- */
static void test_child_exits_first(void) {
    long child = sys_fork();
    if (child == 0) {
        sys_exit(11);
    }

    /* Give child time to exit completely */
    for (int i = 0; i < 20; i++) sys_yield();

    /* waitpid should return immediately since child already exited */
    long st = sys_waitpid(child);
    TEST("waitpid for already-exited child returns status (11)", st == 11);
}

int main(void) {
    printf("=== Stage 68: SMP Race Fix (waitpid sleep/wake) ===\n");

    test_waitpid_basic();
    test_waitpid_sigchld_no_eintr();
    test_waitpid_other_signal_eintr();
    test_rapid_fork_wait();
    test_multi_child_wait();
    test_wnohang();
    test_waitpid_sa_restart();
    test_child_exits_first();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
