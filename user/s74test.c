/* Stage 74 tests: Epoll-driven netagent
 *
 * Tests concurrent TCP client handling via epoll event loop:
 *  - Single client request/response (backward compat)
 *  - Multiple sequential clients
 *  - Concurrent clients via forked children
 *  - Max clients (4) simultaneous connections
 *  - Graceful shutdown after max_requests
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

static long waitpid_retry(long pid) {
    long st;
    while ((st = sys_waitpid(pid)) == -EINTR) ;
    return st;
}

/* Helper: connect to TCP port, send msg, receive response */
static int tcp_request(int port, const char *msg, int msg_len,
                       char *resp, int resp_max) {
    long c = sys_tcp_socket();
    if (c < 0) return -1;

    if (sys_tcp_connect(c, 0x0A00020F, port) != 0) {
        sys_tcp_close(c);
        return -2;
    }

    sys_tcp_send(c, msg, msg_len);
    long n = sys_tcp_recv(c, resp, resp_max - 1);
    if (n > 0) resp[n] = '\0';
    sys_tcp_close(c);
    return (int)n;
}

static int check_response(const char *resp, int n, const char *expected, int exp_len) {
    if (n < 7 + exp_len) return 0;
    /* Check "agent: " prefix */
    if (resp[0] != 'a' || resp[1] != 'g' || resp[2] != 'e' ||
        resp[3] != 'n' || resp[4] != 't' || resp[5] != ':' || resp[6] != ' ')
        return 0;
    /* Check payload */
    for (int i = 0; i < exp_len; i++) {
        if (resp[7 + i] != expected[i]) return 0;
    }
    return 1;
}

/* --- Test 1: single request backward compat --- */
static void test_single_request(void) {
    const char *argv[] = {"netagent.elf", "9740", "5", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    char resp[256];
    int n = tcp_request(9740, "hello", 5, resp, sizeof(resp));
    TEST("single request: response received", n > 0);
    TEST("single request: correct response", check_response(resp, n, "hello", 5));

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

/* --- Test 2: multiple sequential requests --- */
static void test_sequential(void) {
    const char *argv[] = {"netagent.elf", "9741", "10", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("sequential: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    int ok_count = 0;
    for (int i = 0; i < 4; i++) {
        char msg[16];
        msg[0] = 'r';
        msg[1] = 'e';
        msg[2] = 'q';
        msg[3] = '0' + (char)i;
        msg[4] = '\0';

        char resp[256];
        int n = tcp_request(9741, msg, 4, resp, sizeof(resp));
        if (n > 0 && check_response(resp, n, msg, 4))
            ok_count++;
    }
    TEST("sequential: 4 requests all correct", ok_count == 4);

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

/* --- Test 3: concurrent clients via fork --- */
static void test_concurrent(void) {
    const char *argv[] = {"netagent.elf", "9742", "20", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("concurrent: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Fork 3 children that each send a request concurrently */
    long child_pids[3];
    for (int i = 0; i < 3; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            /* Child: send request, exit 0 on success, 1 on fail */
            char msg[8];
            msg[0] = 'c';
            msg[1] = '0' + (char)i;
            msg[2] = '\0';

            char resp[256];
            int n = tcp_request(9742, msg, 2, resp, sizeof(resp));
            if (n > 0 && check_response(resp, n, msg, 2))
                sys_exit(0);
            sys_exit(1);
        }
        child_pids[i] = pid;
    }

    /* Wait for all children */
    int success_count = 0;
    for (int i = 0; i < 3; i++) {
        long st = waitpid_retry(child_pids[i]);
        if (st == 0) success_count++;
    }
    TEST("concurrent: 3 clients all succeed", success_count == 3);

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

/* --- Test 4: max clients (4 simultaneous) --- */
static void test_max_clients(void) {
    const char *argv[] = {"netagent.elf", "9743", "20", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("max_clients: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Fork 4 children — all MAX_CLIENTS slots should be used */
    long child_pids[4];
    for (int i = 0; i < 4; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            char msg[8];
            msg[0] = 'm';
            msg[1] = '0' + (char)i;
            msg[2] = '\0';

            char resp[256];
            int n = tcp_request(9743, msg, 2, resp, sizeof(resp));
            if (n > 0 && check_response(resp, n, msg, 2))
                sys_exit(0);
            sys_exit(1);
        }
        child_pids[i] = pid;
    }

    int success_count = 0;
    for (int i = 0; i < 4; i++) {
        long st = waitpid_retry(child_pids[i]);
        if (st == 0) success_count++;
    }
    TEST("max_clients: 4 clients all succeed", success_count == 4);

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

/* --- Test 5: graceful shutdown on max_requests --- */
static void test_graceful_shutdown(void) {
    /* netagent with max_requests=3 should exit cleanly after 3 */
    const char *argv[] = {"netagent.elf", "9744", "3", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("shutdown: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Send 3 requests */
    for (int i = 0; i < 3; i++) {
        char resp[256];
        tcp_request(9744, "x", 1, resp, sizeof(resp));
    }

    /* Give netagent time to notice it hit max and exit */
    wait_ticks(50);

    /* Reap — should exit 0 */
    long st = waitpid_retry(agent_pid);
    TEST("shutdown: netagent exits cleanly (status 0)", st == 0);
}

/* --- Test 6: epoll health heartbeats continue during idle --- */
static void test_health_reporting(void) {
    const char *argv[] = {"netagent.elf", "9745", "5", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("health: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Check that netagent is reachable via infer_route */
    long routed = sys_infer_route("netagent");
    TEST("health: netagent registered in inference service", routed > 0);

    /* Send a request to verify it's functional */
    char resp[256];
    int n = tcp_request(9745, "ping", 4, resp, sizeof(resp));
    TEST("health: responds while registered", n > 0);

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

/* --- Test 7: backward compat with s71-style patterns --- */
static void test_backward_compat(void) {
    const char *argv[] = {"netagent.elf", "9746", "5", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("compat: netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(50);

    /* Same test pattern as s71test: send "hello", expect "agent: hello" */
    char resp[256];
    int n = tcp_request(9746, "hello", 5, resp, sizeof(resp));
    TEST("compat: receives response", n > 0);

    if (n > 0) {
        int prefix_ok = check_response(resp, n, "hello", 5);
        TEST("compat: s71-compatible response format", prefix_ok);
    } else {
        TEST("compat: s71-compatible response format", 0);
    }

    sys_kill(agent_pid, 9);
    waitpid_retry(agent_pid);
}

int main(void) {
    printf("=== Stage 74: Epoll-Driven Netagent ===\n");

    test_single_request();
    test_sequential();
    test_concurrent();
    test_max_clients();
    test_graceful_shutdown();
    test_health_reporting();
    test_backward_compat();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
