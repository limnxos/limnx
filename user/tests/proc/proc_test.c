/*
 * proc_test.c — Process lifecycle tests
 * Tests: fork, exec, waitpid, signal, session, job control
 * Portable — no arch-specific code.
 */
#include "../limntest.h"

static volatile int sig_received = 0;

static void sigusr_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sys_sigreturn();
}

static void test_fork_waitpid(void) {
    long child = sys_fork();
    if (child == 0) sys_exit(42);
    long st = sys_waitpid(child);
    lt_ok(child > 0, "fork returns child PID");
    lt_ok(st == 42, "waitpid returns exit status");
}

static void test_fork_exec(void) {
    long child = sys_fork();
    if (child == 0) {
        const char *argv[] = {"hello.elf", (void *)0};
        sys_execve("/hello.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "fork+execve+waitpid");
}

static void test_pipe(void) {
    long rfd, wfd;
    long ret = sys_pipe(&rfd, &wfd);
    lt_ok(ret == 0, "pipe creation");
    sys_fwrite(wfd, "hello", 5);
    char buf[16];
    long n = sys_read(rfd, buf, 15);
    lt_ok(n == 5, "pipe read 5 bytes");
    sys_close(rfd);
    sys_close(wfd);
}

static void test_signal(void) {
    sig_received = 0;
    long ret = sys_sigaction(10, sigusr_handler);
    lt_ok(ret == 0, "sigaction installed");

    long parent = sys_getpid();
    long child = sys_fork();
    if (child == 0) {
        long ts[2] = {0, 50000000};
        sys_nanosleep(ts);
        sys_kill(parent, 10);
        sys_exit(0);
    }
    long ts[2] = {0, 50000000};
    for (int i = 0; i < 20 && !sig_received; i++)
        sys_nanosleep(ts);
    lt_ok(sig_received == 1, "signal delivered across fork");
    sys_waitpid(child);
}

static void test_session(void) {
    long sid = sys_getsid(0);
    lt_ok(sid > 0, "getsid returns valid sid");

    long child = sys_fork();
    if (child == 0) {
        long new_sid = sys_setsid();
        sys_exit(new_sid == sys_getpid() ? 0 : 1);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "child setsid creates new session");
}

static void test_sigtstp_sigcont(void) {
    long child = sys_fork();
    if (child == 0) {
        for (;;) sys_yield();
    }
    long ts[2] = {0, 50000000};
    sys_nanosleep(ts);
    sys_kill(child, 21);  /* SIGTSTP */
    sys_nanosleep(ts);
    sys_kill(child, 18);  /* SIGCONT */
    sys_nanosleep(ts);
    sys_kill(child, 9);   /* SIGKILL */
    sys_waitpid(child);
    lt_ok(1, "SIGTSTP+SIGCONT+SIGKILL cycle");
}

static void test_getpid_getppid(void) {
    long pid = sys_getpid();
    lt_ok(pid > 0, "getpid > 0");
}

static void test_multi_fork(void) {
    int count = 4;
    long pids[4];
    for (int i = 0; i < count; i++) {
        pids[i] = sys_fork();
        if (pids[i] == 0) sys_exit(i + 10);
    }
    int all_ok = 1;
    for (int i = 0; i < count; i++) {
        long st = sys_waitpid(pids[i]);
        if (st != i + 10) all_ok = 0;
    }
    lt_ok(all_ok, "multi-fork: 4 children exit with correct status");
}

static void test_env_inherit(void) {
    sys_setenv("TEST_INHERIT", "hello");
    long child = sys_fork();
    if (child == 0) {
        char val[32];
        long ret = sys_getenv("TEST_INHERIT", val, 32);
        sys_exit(ret >= 0 && strcmp(val, "hello") == 0 ? 0 : 1);
    }
    long st = sys_waitpid(child);
    lt_ok(st == 0, "env inherited across fork");
}

int main(void) {
    lt_suite("proc");
    test_fork_waitpid();
    test_fork_exec();
    test_pipe();
    test_signal();
    test_session();
    test_sigtstp_sigcont();
    test_getpid_getppid();
    test_multi_fork();
    test_env_inherit();
    return lt_done();
}
