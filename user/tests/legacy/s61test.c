#include "libc/libc.h"

static int passed = 0;
static int failed = 0;

static void check(int ok, const char *name) {
    if (ok) {
        printf("  PASS: %s\n", name);
        passed++;
    } else {
        printf("  FAIL: %s\n", name);
        failed++;
    }
}

/* Test 1: Rapid fork+exit — fork 20 children, each exits immediately */
static void test_rapid_fork(void) {
    printf("[group 1] Rapid fork+exit (20 children)\n");
    long pids[20];
    int n = 20;

    for (int i = 0; i < n; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: exit immediately with status i+1 */
            sys_exit(i + 1);
        }
        pids[i] = pid;
    }

    /* Parent: waitpid all children */
    int all_ok = 1;
    for (int i = 0; i < n; i++) {
        if (pids[i] < 0) {
            all_ok = 0;
            continue;
        }
        long st = sys_waitpid(pids[i]);
        if (st != i + 1) all_ok = 0;
    }
    check(all_ok, "20 children forked and reaped");
}

/* Test 2: Fork+exec — children exec a program and exit */
static void test_fork_exec(void) {
    printf("[group 2] Fork+exec (5 children exec hello.elf)\n");
    long pids[5];
    int n = 5;

    for (int i = 0; i < n; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            const char *argv[] = {"hello.elf", NULL};
            long child_pid = sys_exec("/hello.elf", argv);
            if (child_pid > 0)
                sys_waitpid(child_pid);
            sys_exit(0);
        }
        pids[i] = pid;
    }

    int all_ok = 1;
    for (int i = 0; i < n; i++) {
        if (pids[i] < 0) { all_ok = 0; continue; }
        long st = sys_waitpid(pids[i]);
        if (st != 0) all_ok = 0;
    }
    check(all_ok, "5 fork+exec children completed");
}

/* Test 3: Nested fork — child forks grandchild */
static void test_nested_fork(void) {
    printf("[group 3] Nested fork (parent→child→grandchild)\n");

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: fork grandchild */
        long gpid = sys_fork();
        if (gpid == 0) {
            /* Grandchild: exit with 42 */
            sys_exit(42);
        }
        long gst = sys_waitpid(gpid);
        /* Child exits with grandchild's status */
        sys_exit((int)gst);
    }
    long st = sys_waitpid(pid);
    check(st == 42, "nested fork: grandchild status propagated");
}

/* Test 4: Concurrent forkers — fork 5 children, each forks 4 grandchildren */
static void test_concurrent_forkers(void) {
    printf("[group 4] Concurrent forkers (5 parents × 4 children)\n");
    long pids[5];

    for (int i = 0; i < 5; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Each child forks 4 grandchildren */
            long gpids[4];
            for (int j = 0; j < 4; j++) {
                long gp = sys_fork();
                if (gp == 0) {
                    /* Grandchild: small busy work then exit */
                    volatile int sum = 0;
                    for (int k = 0; k < 100; k++) sum += k;
                    (void)sum;
                    sys_exit(0);
                }
                gpids[j] = gp;
            }
            /* Wait for all grandchildren */
            int ok = 1;
            for (int j = 0; j < 4; j++) {
                if (gpids[j] < 0) { ok = 0; continue; }
                sys_waitpid(gpids[j]);
            }
            sys_exit(ok ? 0 : 1);
        }
        pids[i] = pid;
    }

    int all_ok = 1;
    for (int i = 0; i < 5; i++) {
        if (pids[i] < 0) { all_ok = 0; continue; }
        long st = sys_waitpid(pids[i]);
        if (st != 0) all_ok = 0;
    }
    check(all_ok, "5×4 concurrent fork tree completed");
}

/* Test 5: Rapid fork+exit rounds — repeat 3 times to catch intermittent issues */
static void test_repeated_rounds(void) {
    printf("[group 5] Repeated rapid fork (3 rounds × 15 children)\n");
    int rounds_ok = 1;

    for (int round = 0; round < 3; round++) {
        long pids[15];
        for (int i = 0; i < 15; i++) {
            long pid = sys_fork();
            if (pid == 0) {
                sys_exit(round * 100 + i);
            }
            pids[i] = pid;
        }
        for (int i = 0; i < 15; i++) {
            if (pids[i] < 0) { rounds_ok = 0; continue; }
            long st = sys_waitpid(pids[i]);
            if (st != round * 100 + i) rounds_ok = 0;
        }
    }
    check(rounds_ok, "3 rounds of 15 fork+exit");
}

/* Test 6: Fork with SIGCHLD — verify SIGCHLD delivery on child exit */
static volatile int sigchld_count = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    sigchld_count++;
    sys_sigreturn();
}

static void test_sigchld_storm(void) {
    printf("[group 6] SIGCHLD storm (10 children with handler)\n");
    sigchld_count = 0;

    sys_sigaction(SIGCHLD, sigchld_handler);

    long pids[10];
    for (int i = 0; i < 10; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            sys_exit(0);
        }
        pids[i] = pid;
    }

    for (int i = 0; i < 10; i++) {
        if (pids[i] > 0)
            sys_waitpid(pids[i]);
    }

    /* Reset handler */
    sys_sigaction(SIGCHLD, NULL);

    /* sigchld_count may not be exactly 10 due to coalescing, but should be > 0 */
    check(sigchld_count > 0, "SIGCHLD delivered during fork storm");
}

/* Test 7: WNOHANG polling — fork child, poll with WNOHANG until done */
static void test_wnohang_poll(void) {
    printf("[group 7] WNOHANG polling\n");

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: small delay via yield, then exit */
        for (int i = 0; i < 50; i++)
            sys_yield();
        sys_exit(77);
    }

    /* Poll with WNOHANG */
    int polls = 0;
    long st;
    for (;;) {
        st = sys_waitpid_flags(pid, WNOHANG);
        if (st != 0) break;
        polls++;
        sys_yield();
        if (polls > 10000) break;  /* safety limit */
    }
    check(st == 77, "WNOHANG poll got correct exit status");
    check(polls > 0, "WNOHANG returned 0 at least once before completion");
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== Stage 61: SMP fork stress tests ===\n\n");

    test_rapid_fork();
    test_fork_exec();
    test_nested_fork();
    test_concurrent_forkers();
    test_repeated_rounds();
    test_sigchld_storm();
    test_wnohang_poll();

    printf("\n=== Stage 61 Results: %d/%d passed ===\n",
           passed, passed + failed);

    return failed > 0 ? 1 : 0;
}
