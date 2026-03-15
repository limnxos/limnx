/*
 * s100test.c — Stage 100: Fork+Exec End-to-End Test
 *
 * Tests: fork PID semantics, pipe IPC, fork+exec+waitpid,
 *        signal delivery across fork, multi-fork stress.
 * Portable — no arch-specific code.
 */

#include "../../libc/libc.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        tests_failed++;
    }
}

/* --- Signal handler for Test 4 --- */

static volatile int sig_received = 0;

static void sigusr1_handler(int signum) {
    (void)signum;
    sig_received = 1;
    sys_sigreturn();
}

/* Brief sleep (50ms) to let scheduler run other processes */
static void brief_sleep(void) {
    long ts[2];          /* [0]=tv_sec, [1]=tv_nsec */
    ts[0] = 0;
    ts[1] = 50000000;    /* 50ms */
    sys_nanosleep(ts);
}

/* --- Tests --- */

static void test_fork_pids(void) {
    printf("[1] Fork PID verification\n");

    long parent_pid = sys_getpid();
    check("parent PID > 0", parent_pid > 0);

    long child_pid = sys_fork();

    if (child_pid == 0) {
        /* Child */
        long my_pid = sys_getpid();
        /* Child PID should differ from parent */
        if (my_pid != parent_pid)
            printf("  PASS: child PID (%ld) != parent PID (%ld)\n",
                   my_pid, parent_pid);
        else
            printf("  FAIL: child PID == parent PID\n");
        sys_exit(42);
    }

    /* Parent */
    check("fork returned child PID > 0", child_pid > 0);

    long status = sys_waitpid(child_pid);
    check("waitpid returned child exit status", status == 42);
}

static void test_pipe_ipc(void) {
    printf("[2] Pipe IPC between parent and child\n");

    long rfd = -1, wfd = -1;
    long ret = sys_pipe(&rfd, &wfd);
    check("pipe creation", ret == 0);
    check("pipe read fd valid", rfd >= 0);
    check("pipe write fd valid", wfd >= 0);

    long child_pid = sys_fork();

    if (child_pid == 0) {
        /* Child: close read end, write message, close write end */
        sys_close(rfd);
        const char *msg = "hello";
        sys_fwrite(wfd, msg, 5);
        sys_close(wfd);
        sys_exit(0);
    }

    /* Parent: close write end first so pipe detects EOF correctly */
    sys_close(wfd);

    /* Wait for child to finish writing and exit.
     * This eliminates SMP race conditions — the child has definitely
     * written to the pipe and closed its write end before we read. */
    sys_waitpid(child_pid);

    /* Now read — pipe has data and closed_write is true */
    char buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0;
    long n = sys_read(rfd, buf, 16);
    sys_close(rfd);

    check("read 5 bytes from pipe", n == 5);
    check("pipe data correct", buf[0] == 'h' && buf[1] == 'e' &&
          buf[2] == 'l' && buf[3] == 'l' && buf[4] == 'o');
}

static void test_fork_exec(void) {
    printf("[3] Fork + execve + waitpid\n");

    long child_pid = sys_fork();

    if (child_pid == 0) {
        /* Child: execve replaces this process with hello.elf */
        sys_execve("/hello.elf", 0);
        /* If execve fails, exit with error */
        sys_exit(99);
    }

    check("fork for exec returned child PID > 0", child_pid > 0);

    long status = sys_waitpid(child_pid);
    check("exec'd child exited cleanly", status == 0);
}

static void test_signal_across_fork(void) {
    printf("[4] Signal delivery across fork\n");

    sig_received = 0;
    long ret = sys_sigaction(10, sigusr1_handler);  /* SIGUSR1 = 10 */
    check("sigaction installed", ret == 0);

    long parent_pid = sys_getpid();
    long child_pid = sys_fork();

    if (child_pid == 0) {
        /* Child: sleep briefly to let parent enter wait loop,
         * then send SIGUSR1 to parent */
        brief_sleep();
        sys_kill(parent_pid, 10);
        sys_exit(0);
    }

    /* Parent: wait for signal with proper timeout (500ms) */
    for (int i = 0; i < 10 && !sig_received; i++)
        brief_sleep();

    check("SIGUSR1 received from child", sig_received == 1);

    sys_waitpid(child_pid);
}

static void test_multi_fork(void) {
    printf("[5] Multi-fork stress test\n");

    #define NUM_CHILDREN 4
    long pids[NUM_CHILDREN];
    int all_forked = 1;

    for (int i = 0; i < NUM_CHILDREN; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: exit with unique status */
            sys_exit(10 + i);
        }
        if (pid <= 0)
            all_forked = 0;
        pids[i] = pid;
    }

    check("all 4 children forked", all_forked);

    int all_reaped = 1;
    for (int i = 0; i < NUM_CHILDREN; i++) {
        long status = sys_waitpid(pids[i]);
        if (status != 10 + i) {
            printf("  FAIL: child %d exit status %ld (expected %d)\n",
                   i, status, 10 + i);
            all_reaped = 0;
        }
    }

    check("all 4 children reaped with correct status", all_reaped);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== Stage 100: Fork+Exec End-to-End Test ===\n\n");

    test_fork_pids();
    test_pipe_ipc();
    test_fork_exec();
    test_signal_across_fork();
    test_multi_fork();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");
    else
        printf("SOME TESTS FAILED\n");

    sys_exit(tests_failed > 0 ? 1 : 0);
    return 0;
}
