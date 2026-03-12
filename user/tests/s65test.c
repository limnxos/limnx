/* Stage 65 tests: pipe2, SA_RESTART, supervisor restart */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

/* ---- pipe2 tests ---- */

static void test_pipe2_basic(void) {
    long rfd, wfd;
    long r = sys_pipe2(&rfd, &wfd, 0);
    TEST("pipe2(0) succeeds", r == 0);

    sys_fwrite(wfd, "test", 4);
    char buf[8] = {0};
    long n = sys_read(rfd, buf, 4);
    TEST("pipe2 read/write works", n == 4 && buf[0] == 't');

    sys_close(rfd);
    sys_close(wfd);
}

static void test_pipe2_cloexec(void) {
    long rfd, wfd;
    sys_pipe2(&rfd, &wfd, 0x01);  /* O_CLOEXEC */

    long rflags = sys_fcntl(rfd, 1, 0);  /* F_GETFD */
    long wflags = sys_fcntl(wfd, 1, 0);
    TEST("pipe2 O_CLOEXEC sets FD_CLOEXEC on read end", (rflags & 0x01) != 0);
    TEST("pipe2 O_CLOEXEC sets FD_CLOEXEC on write end", (wflags & 0x01) != 0);

    sys_close(rfd);
    sys_close(wfd);
}

static void test_pipe2_nonblock(void) {
    long rfd, wfd;
    sys_pipe2(&rfd, &wfd, 0x800);  /* O_NONBLOCK */

    /* Read from empty pipe should return -EAGAIN (not block) */
    char buf[8];
    long n = sys_read(rfd, buf, 4);
    TEST("pipe2 O_NONBLOCK read returns -EAGAIN", n < 0);

    sys_close(rfd);
    sys_close(wfd);
}

static void test_pipe2_both_flags(void) {
    long rfd, wfd;
    sys_pipe2(&rfd, &wfd, 0x01 | 0x800);  /* O_CLOEXEC | O_NONBLOCK */

    long rflags = sys_fcntl(rfd, 1, 0);
    TEST("pipe2 both flags: cloexec set", (rflags & 0x01) != 0);

    char buf[8];
    long n = sys_read(rfd, buf, 4);
    TEST("pipe2 both flags: nonblock works", n < 0);

    sys_close(rfd);
    sys_close(wfd);
}

/* ---- SA_RESTART tests ---- */

static volatile int sig_received = 0;

static void restart_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sys_sigreturn();
}

static void test_sa_restart_pipe(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    /* Set up SIGINT handler with SA_RESTART */
    sys_sigaction3(SIGINT, restart_handler, SA_RESTART);
    sig_received = 0;

    long parent_pid = sys_getpid();
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: wait a bit, send SIGINT to parent, then write data */
        sys_close(rfd);
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(parent_pid, SIGINT);
        for (int i = 0; i < 5; i++) sys_yield();
        sys_fwrite(wfd, "ok", 2);
        sys_close(wfd);
        sys_exit(0);
    }

    /* Parent: read from pipe (will block, get interrupted, restart) */
    sys_close(wfd);
    char buf[8] = {0};
    long n = sys_read(rfd, buf, 2);
    TEST("SA_RESTART: read resumes after signal", n == 2 && buf[0] == 'o');
    TEST("SA_RESTART: signal handler was called", sig_received == 1);

    sys_close(rfd);
    sys_waitpid(pid);
}

static void no_restart_handler(int sig) {
    (void)sig;
    sig_received = 1;
    sys_sigreturn();
}

static void test_no_restart_pipe(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    /* Set up SIGINT handler WITHOUT SA_RESTART */
    sys_sigaction3(SIGINT, no_restart_handler, 0);
    sig_received = 0;

    long parent_pid2 = sys_getpid();
    long pid = sys_fork();
    if (pid == 0) {
        sys_close(rfd);
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(parent_pid2, SIGINT);
        for (int i = 0; i < 10; i++) sys_yield();
        sys_fwrite(wfd, "ok", 2);
        sys_close(wfd);
        sys_exit(0);
    }

    sys_close(wfd);
    char buf[8] = {0};
    long n = sys_read(rfd, buf, 2);
    /* Without SA_RESTART, read should return -EINTR (negative) */
    TEST("no SA_RESTART: read returns error on signal", n < 0);
    TEST("no SA_RESTART: signal handler was called", sig_received == 1);

    sys_close(rfd);
    sys_waitpid(pid);

    /* Reset handler */
    sys_sigaction(SIGINT, SIG_DFL);
}

/* ---- Supervisor restart test ---- */

static volatile int sigchld_count = 0;

static void sigchld_handler(int sig) {
    (void)sig;
    sigchld_count++;
    sys_sigreturn();
}

static void test_supervisor_restart(void) {
    /* Set up SIGCHLD handler to count restarts */
    sys_sigaction(SIGCHLD, sigchld_handler);
    sigchld_count = 0;

    long sup_id = sys_super_create("restart_test");
    TEST("supervisor create for restart", sup_id >= 0);

    long child_idx = sys_super_add(sup_id, "/crasher.elf", -1, 0xFF);
    TEST("supervisor add crasher child", child_idx >= 0);

    long launched = sys_super_start(sup_id);
    TEST("supervisor start launches child", launched == 1);

    /* Wait for some restarts — crasher exits immediately with status 1,
     * supervisor auto-restarts it. Each crash+restart produces SIGCHLD. */
    for (int i = 0; i < 500; i++)
        sys_yield();

    /* We should have seen multiple SIGCHLD signals from restarts */
    TEST("supervisor restarted crasher (SIGCHLD count >= 2)", sigchld_count >= 2);

    /* Reset handler */
    sys_sigaction(SIGCHLD, SIG_DFL);
}

int main(void) {
    printf("=== Stage 65 Tests: pipe2 + SA_RESTART + supervisor restart ===\n");

    /* pipe2 tests */
    test_pipe2_basic();
    test_pipe2_cloexec();
    test_pipe2_nonblock();
    test_pipe2_both_flags();

    /* SA_RESTART tests */
    test_sa_restart_pipe();
    test_no_restart_pipe();

    /* Supervisor restart test */
    test_supervisor_restart();

    printf("=== Results: %d/%d passed ===\n", pass, pass + fail);
    if (fail == 0) printf("ALL TESTS PASSED\n");
    return fail;
}
