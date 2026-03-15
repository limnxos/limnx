/* Stage 66 tests: extended -EINTR for all blocking syscalls */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

static volatile int got_signal = 0;

static void test_handler(int sig) {
    (void)sig;
    got_signal = 1;
    sys_sigreturn();
}

/* Helper: fork a child that sends SIGINT to parent after a few yields */
static long fork_signaler(long parent_pid) {
    long pid = sys_fork();
    if (pid == 0) {
        for (int i = 0; i < 5; i++) sys_yield();
        sys_kill(parent_pid, SIGINT);
        sys_exit(0);
    }
    return pid;
}

/* --- Test: pipe write -EINTR --- */
static void test_pipe_write_eintr(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    /* Fill the pipe buffer to make write block */
    char buf[128];
    for (int i = 0; i < 128; i++) buf[i] = 'x';
    /* Write until it would block (pipe is 4096 bytes) */
    for (int i = 0; i < 32; i++)
        sys_fwrite(wfd, buf, 128);

    /* Now pipe is full. Fork a child to send us SIGINT */
    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* This write should block (pipe full) then get -EINTR */
    long n = sys_fwrite(wfd, buf, 128);
    TEST("pipe write returns -EINTR when signaled", n == -EINTR);
    TEST("pipe write: signal handler was called", got_signal == 1);

    sys_close(rfd);
    sys_close(wfd);
    sys_waitpid(child);
}

/* --- Test: nanosleep -EINTR --- */
static void test_nanosleep_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* Sleep for a long time — should be interrupted */
    long ts[2] = {10, 0};  /* 10 seconds */
    long r = sys_nanosleep(ts);
    TEST("nanosleep returns -EINTR when signaled", r == -EINTR);
    TEST("nanosleep: signal handler was called", got_signal == 1);

    sys_waitpid(child);
}

/* --- Test: poll -EINTR --- */
static void test_poll_eintr(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* Poll with infinite timeout — should be interrupted */
    struct { int fd; short events; short revents; } pfd;
    pfd.fd = (int)rfd;
    pfd.events = 0x001;  /* POLLIN */
    pfd.revents = 0;
    long r = sys_poll(&pfd, 1, -1);
    TEST("poll returns -EINTR when signaled", r == -EINTR);
    TEST("poll: signal handler was called", got_signal == 1);

    sys_close(rfd);
    sys_close(wfd);
    sys_waitpid(child);
}

/* --- Test: select -EINTR --- */
static void test_select_eintr(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* Select with no timeout (infinite wait) — should be interrupted */
    fd_set_t readfds;
    FD_ZERO(&readfds);
    FD_SET(rfd, &readfds);
    long r = sys_select(rfd + 1, &readfds, NULL, -1);
    TEST("select returns -EINTR when signaled", r == -EINTR);
    TEST("select: signal handler was called", got_signal == 1);

    sys_close(rfd);
    sys_close(wfd);
    sys_waitpid(child);
}

/* --- Test: epoll_wait -EINTR --- */
static void test_epoll_eintr(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    long epfd = sys_epoll_create(0);
    TEST("epoll_create succeeds", epfd >= 0);
    if (epfd < 0) return;

    epoll_event_t ev;
    ev.events = 0x001;  /* EPOLLIN */
    ev.data = (uint64_t)rfd;
    sys_epoll_ctl(epfd, 1, rfd, &ev);  /* EPOLL_CTL_ADD */

    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* epoll_wait with infinite timeout — should be interrupted */
    epoll_event_t out[4];
    long r = sys_epoll_wait(epfd, out, 4, -1);
    TEST("epoll_wait returns -EINTR when signaled", r == -EINTR);
    TEST("epoll_wait: signal handler was called", got_signal == 1);

    sys_close(epfd);
    sys_close(rfd);
    sys_close(wfd);
    sys_waitpid(child);
}

/* --- Test: unix accept -EINTR --- */
static void test_unix_accept_eintr(void) {
    long sfd = sys_unix_socket();
    TEST("unix socket create", sfd >= 0);
    if (sfd < 0) return;

    sys_unix_bind(sfd, "/tmp/s66test.sock");
    sys_unix_listen(sfd);

    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* Accept with no connection — should block then get -EINTR */
    long r = sys_unix_accept(sfd);
    TEST("unix_accept returns -EINTR when signaled", r == -EINTR);
    TEST("unix_accept: signal handler was called", got_signal == 1);

    sys_close(sfd);
    sys_waitpid(child);
}

/* --- Test: SA_RESTART with nanosleep --- */
static void test_sa_restart_nanosleep(void) {
    sys_sigaction3(SIGINT, test_handler, SA_RESTART);
    got_signal = 0;

    long ppid = sys_getpid();
    long child = fork_signaler(ppid);

    /* With SA_RESTART, nanosleep should resume after signal.
     * Use a short sleep so the test doesn't take too long. */
    long ts[2] = {0, 200000000};  /* 0.2 seconds */
    long r = sys_nanosleep(ts);
    /* SA_RESTART should cause it to restart and complete successfully */
    TEST("SA_RESTART nanosleep completes after signal", r == 0);
    TEST("SA_RESTART nanosleep: signal handler was called", got_signal == 1);

    sys_waitpid(child);
}

int main(void) {
    printf("=== Stage 66 Tests: extended -EINTR for blocking syscalls ===\n");

    test_pipe_write_eintr();
    test_nanosleep_eintr();
    test_poll_eintr();
    test_select_eintr();
    test_epoll_eintr();
    test_unix_accept_eintr();
    test_sa_restart_nanosleep();

    /* Reset handler */
    sys_sigaction(SIGINT, SIG_DFL);

    printf("=== Results: %d/%d passed ===\n", pass, pass + fail);
    if (fail == 0) printf("ALL TESTS PASSED\n");
    return fail;
}
