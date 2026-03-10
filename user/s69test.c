/* Stage 69 tests: TCP/UDP -EINTR for blocking network syscalls */
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
static long fork_signaler(long parent_pid, int delay) {
    long pid = sys_fork();
    if (pid == 0) {
        for (int i = 0; i < delay; i++) sys_yield();
        sys_kill(parent_pid, SIGINT);
        sys_exit(0);
    }
    return pid;
}

/* --- Test 1: TCP recv -EINTR --- */
static void test_tcp_recv_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    /* Create a TCP listener */
    long srv = sys_tcp_socket();
    TEST("tcp_socket for server", srv >= 0);
    if (srv < 0) return;

    long r = sys_tcp_listen(srv, 9100);
    TEST("tcp_listen on 9100", r == 0);
    if (r != 0) { sys_tcp_close(srv); return; }

    /* Fork a client that connects and holds the connection open */
    long client_pid = sys_fork();
    if (client_pid == 0) {
        sys_yield(); sys_yield();
        long c = sys_tcp_socket();
        sys_tcp_connect(c, 0x0A00020F, 9100);  /* 10.0.2.15 (our IP) */
        /* Don't send any data — just hold connection open */
        for (int i = 0; i < 50; i++) sys_yield();
        sys_tcp_close(c);
        sys_exit(0);
    }

    /* Accept the connection */
    long conn = sys_tcp_accept(srv);
    TEST("tcp_accept succeeds", conn >= 0);
    if (conn < 0) {
        sys_tcp_close(srv);
        sys_waitpid(client_pid);
        return;
    }

    /* Now signal ourselves while blocked in tcp_recv */
    long ppid = sys_getpid();
    long sig_pid = fork_signaler(ppid, 5);

    char buf[64];
    long n = sys_tcp_recv(conn, buf, sizeof(buf));
    TEST("tcp_recv returns -EINTR when signaled", n == -EINTR);
    TEST("tcp_recv: signal handler called", got_signal == 1);

    sys_tcp_close(conn);
    sys_tcp_close(srv);
    sys_waitpid(client_pid);
    sys_waitpid(sig_pid);
}

/* --- Test 2: TCP accept -EINTR --- */
static void test_tcp_accept_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long srv = sys_tcp_socket();
    TEST("tcp_socket for accept test", srv >= 0);
    if (srv < 0) return;

    long r = sys_tcp_listen(srv, 9101);
    TEST("tcp_listen on 9101", r == 0);
    if (r != 0) { sys_tcp_close(srv); return; }

    /* Nobody will connect — signal will interrupt the accept */
    long ppid = sys_getpid();
    long sig_pid = fork_signaler(ppid, 5);

    long conn = sys_tcp_accept(srv);
    TEST("tcp_accept returns -EINTR when signaled", conn == -EINTR);
    TEST("tcp_accept: signal handler called", got_signal == 1);

    sys_tcp_close(srv);
    sys_waitpid(sig_pid);
}

/* --- Test 3: TCP connect -EINTR --- */
static void test_tcp_connect_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long c = sys_tcp_socket();
    TEST("tcp_socket for connect test", c >= 0);
    if (c < 0) return;

    /* Connect to a port nobody is listening on — will SYN and wait */
    long ppid = sys_getpid();
    long sig_pid = fork_signaler(ppid, 5);

    /* Use a non-routable IP to ensure connect blocks (10.0.2.200) */
    long r = sys_tcp_connect(c, 0x0A0002C8, 9999);
    TEST("tcp_connect returns -EINTR when signaled", r == -EINTR);
    TEST("tcp_connect: signal handler called", got_signal == 1);

    sys_tcp_close(c);
    sys_waitpid(sig_pid);
}

/* --- Test 4: UDP recvfrom -EINTR --- */
static void test_udp_recv_eintr(void) {
    sys_sigaction3(SIGINT, test_handler, 0);
    got_signal = 0;

    long s = sys_socket();
    TEST("udp socket create", s >= 0);
    if (s < 0) return;

    long r = sys_bind(s, 9200);
    TEST("udp bind to 9200", r == 0);
    if (r != 0) return;

    /* Nobody will send us data — signal will interrupt */
    long ppid = sys_getpid();
    long sig_pid = fork_signaler(ppid, 5);

    char buf[64];
    unsigned int src_ip;
    unsigned short src_port;
    long n = sys_recvfrom(s, buf, sizeof(buf), &src_ip, &src_port);
    TEST("udp recvfrom returns -EINTR when signaled", n == -EINTR);
    TEST("udp recvfrom: signal handler called", got_signal == 1);

    sys_waitpid(sig_pid);
}

/* --- Test 5: TCP recv without signal works normally --- */
static void test_tcp_recv_normal(void) {
    long srv = sys_tcp_socket();
    if (srv < 0) { TEST("tcp_socket for normal recv", 0); return; }

    sys_tcp_listen(srv, 9102);

    long client_pid = sys_fork();
    if (client_pid == 0) {
        sys_yield(); sys_yield();
        long c = sys_tcp_socket();
        sys_tcp_connect(c, 0x0A00020F, 9102);  /* 10.0.2.15 */
        sys_tcp_send(c, "hello", 5);
        for (int i = 0; i < 10; i++) sys_yield();
        sys_tcp_close(c);
        sys_exit(0);
    }

    long conn = sys_tcp_accept(srv);
    TEST("tcp_accept for normal recv", conn >= 0);
    if (conn < 0) {
        sys_tcp_close(srv);
        sys_waitpid(client_pid);
        return;
    }

    char buf[64];
    long n = sys_tcp_recv(conn, buf, sizeof(buf));
    TEST("tcp_recv returns data normally", n == 5);

    sys_tcp_close(conn);
    sys_tcp_close(srv);
    sys_waitpid(client_pid);
}

int main(void) {
    printf("=== Stage 69: TCP/UDP -EINTR ===\n");

    test_tcp_recv_eintr();
    test_tcp_accept_eintr();
    test_tcp_connect_eintr();
    test_udp_recv_eintr();
    test_tcp_recv_normal();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
