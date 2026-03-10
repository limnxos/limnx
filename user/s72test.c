/* Stage 72 tests: O_NONBLOCK for TCP sockets
 *
 * Tests non-blocking TCP operations and epoll integration:
 *  - tcp_setopt to enable O_NONBLOCK
 *  - Non-blocking accept returns -EAGAIN
 *  - Non-blocking recv returns -EAGAIN
 *  - Non-blocking connect returns -EINPROGRESS
 *  - tcp_to_fd creates a pollable fd
 *  - epoll monitors TCP fd for readiness
 */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

static void wait_ticks(int n) {
    for (int i = 0; i < n; i++) sys_yield();
}

/* --- Test 1: tcp_setopt sets nonblock --- */
static void test_setopt(void) {
    long s = sys_tcp_socket();
    TEST("tcp_socket", s >= 0);
    if (s < 0) return;

    long r = sys_tcp_setopt(s, TCP_OPT_NONBLOCK, 1);
    TEST("tcp_setopt O_NONBLOCK", r == 0);

    /* Invalid opt */
    long r2 = sys_tcp_setopt(s, 99, 1);
    TEST("tcp_setopt invalid opt returns -EINVAL", r2 == -EINVAL);

    sys_tcp_close(s);
}

/* --- Test 2: nonblock accept returns -EAGAIN --- */
static void test_nonblock_accept(void) {
    long s = sys_tcp_socket();
    TEST("listener socket", s >= 0);
    if (s < 0) return;

    long r = sys_tcp_listen(s, 9800);
    TEST("tcp_listen on 9800", r == 0);

    /* Set nonblock on the listener */
    sys_tcp_setopt(s, TCP_OPT_NONBLOCK, 1);

    /* Accept with no pending client → should return -EAGAIN */
    long a = sys_tcp_accept(s);
    TEST("nonblock accept returns -EAGAIN", a == -EAGAIN);

    sys_tcp_close(s);
}

/* --- Test 3: nonblock recv returns -EAGAIN --- */
static void test_nonblock_recv(void) {
    /* Use netagent on port 9801 as server — we connect as client */
    const char *argv[] = {"netagent.elf", "9801", "2", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("recv test: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Connect to netagent */
    long cli = sys_tcp_socket();
    long cr = sys_tcp_connect(cli, 0x0A00020F, 9801);
    TEST("recv test: connected", cr == 0);
    if (cr != 0) {
        sys_tcp_close(cli);
        return;
    }

    /* Set nonblock on client connection */
    sys_tcp_setopt(cli, TCP_OPT_NONBLOCK, 1);

    /* Send request — netagent will respond */
    sys_tcp_send(cli, "test", 4);
    wait_ticks(15);

    /* Data should be available now — nonblock recv gets it */
    char buf[256];
    long n = sys_tcp_recv(cli, buf, sizeof(buf));
    TEST("nonblock recv gets netagent response", n > 0);

    sys_tcp_close(cli);

    /* Connect again, set nonblock, try recv before sending → -EAGAIN */
    long cli2 = sys_tcp_socket();
    sys_tcp_connect(cli2, 0x0A00020F, 9801);
    sys_tcp_setopt(cli2, TCP_OPT_NONBLOCK, 1);

    long n2 = sys_tcp_recv(cli2, buf, sizeof(buf));
    TEST("nonblock recv no data returns -EAGAIN", n2 == -EAGAIN);

    /* Send to exhaust netagent's limit */
    sys_tcp_send(cli2, "done", 4);
    sys_tcp_close(cli2);

    long st;
    while ((st = sys_waitpid(agent_pid)) == -EINTR) ;
}

/* --- Test 4: nonblock connect returns -EINPROGRESS --- */
static void test_nonblock_connect(void) {
    long s = sys_tcp_socket();
    TEST("connect test: socket", s >= 0);
    if (s < 0) return;

    /* Set nonblock before connect */
    sys_tcp_setopt(s, TCP_OPT_NONBLOCK, 1);

    /* Start a listener first so connect has something to reach */
    long srv = sys_tcp_socket();
    sys_tcp_listen(srv, 9802);

    /* Non-blocking connect → should return -EINPROGRESS */
    long r = sys_tcp_connect(s, 0x0A00020F, 9802);
    TEST("nonblock connect returns -EINPROGRESS", r == -EINPROGRESS);

    /* Wait for connection to establish */
    wait_ticks(20);

    /* Accept on server side */
    long a = sys_tcp_accept(srv);
    /* Connection should have completed by now */
    TEST("server accepts after nonblock connect", a >= 0);

    if (a >= 0) sys_tcp_close(a);
    sys_tcp_close(s);
    sys_tcp_close(srv);
}

/* --- Test 5: tcp_to_fd creates pollable fd --- */
static void test_tcp_to_fd(void) {
    long s = sys_tcp_socket();
    TEST("tcp_to_fd: socket", s >= 0);
    if (s < 0) return;

    sys_tcp_listen(s, 9803);

    long fd = sys_tcp_to_fd(s);
    TEST("tcp_to_fd returns valid fd", fd >= 0);

    /* Close the fd — should not close the TCP conn */
    if (fd >= 0) sys_close(fd);

    /* Socket should still work */
    sys_tcp_setopt(s, TCP_OPT_NONBLOCK, 1);
    long a = sys_tcp_accept(s);
    TEST("tcp conn still works after fd close", a == -EAGAIN);

    sys_tcp_close(s);
}

/* --- Test 6: epoll monitors TCP connections via fd --- */
static void test_epoll_tcp(void) {
    /* Use netagent as a remote server, then epoll on the client side */
    const char *argv[] = {"netagent.elf", "9804", "3", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("epoll test: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(30);

    /* Create epoll */
    long epfd = sys_epoll_create(0);
    TEST("epoll test: epoll_create", epfd >= 0);
    if (epfd < 0) return;

    /* Connect to netagent */
    long cli = sys_tcp_socket();
    long cr = sys_tcp_connect(cli, 0x0A00020F, 9804);
    TEST("epoll test: connected", cr == 0);
    if (cr != 0) { sys_close(epfd); sys_tcp_close(cli); return; }

    /* Create fd for client conn + set nonblock */
    long cli_fd = sys_tcp_to_fd(cli);
    sys_tcp_setopt(cli, TCP_OPT_NONBLOCK, 1);
    TEST("epoll test: tcp_to_fd", cli_fd >= 0);

    /* Add client fd to epoll for POLLIN */
    epoll_event_t ev;
    ev.events = EPOLLIN;
    ev.data = (uint64_t)cli_fd;
    long r = sys_epoll_ctl(epfd, EPOLL_CTL_ADD, cli_fd, &ev);
    TEST("epoll_ctl add TCP fd", r == 0);

    /* No data yet → epoll_wait should timeout */
    epoll_event_t out_ev;
    long n = sys_epoll_wait(epfd, &out_ev, 1, 10);
    TEST("epoll_wait no data returns 0", n == 0);

    /* Send request to netagent — it will respond with "agent: req" */
    sys_tcp_send(cli, "req", 3);
    wait_ticks(15);

    /* epoll should now detect data from netagent's response */
    long n2 = sys_epoll_wait(epfd, &out_ev, 1, 100);
    TEST("epoll_wait detects response data", n2 > 0);

    /* Read the response */
    char buf[256];
    long nr = sys_tcp_recv(cli, buf, sizeof(buf));
    TEST("nonblock recv gets response", nr > 0);
    if (nr > 0) {
        buf[nr] = '\0';
        int prefix_ok = (nr >= 7 && buf[0] == 'a' && buf[6] == ' ');
        TEST("response has agent prefix", prefix_ok);
    }

    sys_close(cli_fd);
    sys_tcp_close(cli);

    /* Exhaust netagent's remaining 2 requests */
    char resp[256];
    long c2 = sys_tcp_socket();
    sys_tcp_connect(c2, 0x0A00020F, 9804);
    sys_tcp_send(c2, "x", 1);
    sys_tcp_recv(c2, resp, sizeof(resp));
    sys_tcp_close(c2);

    long c3 = sys_tcp_socket();
    sys_tcp_connect(c3, 0x0A00020F, 9804);
    sys_tcp_send(c3, "x", 1);
    sys_tcp_recv(c3, resp, sizeof(resp));
    sys_tcp_close(c3);

    long st;
    while ((st = sys_waitpid(agent_pid)) == -EINTR) ;

    sys_close(epfd);
}

int main(void) {
    printf("=== Stage 72: O_NONBLOCK for TCP Sockets ===\n");

    test_setopt();
    test_nonblock_accept();
    test_nonblock_recv();
    test_nonblock_connect();
    test_tcp_to_fd();
    test_epoll_tcp();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
