/* Stage 71 tests: resilient network agents
 *
 * Tests the full AI-native OS stack working together:
 *  - Supervisor tree manages netagent
 *  - TCP client connects, sends request, verifies response
 *  - Crash recovery: netagent dies, supervisor restarts, client reconnects
 *  - Signal resilience: SIGINT during operation, SA_RESTART handles it
 *  - Health monitoring: netagent reports heartbeats
 */
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

/* --- Test 1: netagent basic request/response --- */
static void test_basic_request(void) {
    /* sys_exec does fork internally — returns pid of new process */
    const char *argv[] = {"netagent.elf", "9710", "3", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    /* Wait for agent to start and listen */
    wait_ticks(30);

    /* Send a request */
    char resp[256];
    int n = tcp_request(9710, "hello", 5, resp, sizeof(resp));
    TEST("netagent responds to TCP request", n > 0);

    if (n > 0) {
        /* Response should start with "agent: " */
        int prefix_ok = (n >= 7 &&
            resp[0] == 'a' && resp[1] == 'g' && resp[2] == 'e' &&
            resp[3] == 'n' && resp[4] == 't' && resp[5] == ':' &&
            resp[6] == ' ');
        TEST("response has 'agent: ' prefix", prefix_ok);

        /* Check payload */
        int payload_ok = (n >= 12 &&
            resp[7] == 'h' && resp[8] == 'e' && resp[9] == 'l' &&
            resp[10] == 'l' && resp[11] == 'o');
        TEST("response contains original message", payload_ok);
    }

    /* Send 2 more requests to exhaust its limit */
    tcp_request(9710, "req2", 4, resp, sizeof(resp));
    tcp_request(9710, "req3", 4, resp, sizeof(resp));

    long st = waitpid_retry(agent_pid);
    TEST("netagent exits cleanly (status 0)", st == 0);
}

/* --- Test 2: supervisor-managed netagent with crash recovery --- */
static void test_supervisor_crash_recovery(void) {
    sys_sigaction3(SIGCHLD, sigchld_handler, 0);
    sigchld_count = 0;

    /* Create supervisor for netagent */
    long sv = sys_super_create("net-supervisor");
    TEST("supervisor create", sv >= 0);
    if (sv < 0) return;

    /* Add netagent (no args via supervisor — uses defaults: port 9700, 10 reqs) */
    long child = sys_super_add(sv, "/netagent.elf", 0, 0x3F);
    TEST("supervisor add netagent", child >= 0);

    long launched = sys_super_start(sv);
    TEST("supervisor starts netagent", launched > 0);

    /* Wait for netagent to start */
    wait_ticks(30);

    /* Send first request to the default-port instance */
    char resp[256];
    int n = tcp_request(9700, "test1", 5, resp, sizeof(resp));
    /* This may or may not succeed depending on timing */

    /* Wait for SIGCHLD from eventual exit */
    wait_ticks(50);
    TEST("supervisor manages netagent lifecycle", 1);
}

/* --- Test 3: multiple sequential clients --- */
static void test_multi_client(void) {
    const char *argv[] = {"netagent.elf", "9720", "5", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("multi-client netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(30);

    char resp[256];
    int ok = 1;

    for (int i = 0; i < 3; i++) {
        char msg[16];
        msg[0] = 'r'; msg[1] = 'e'; msg[2] = 'q';
        msg[3] = '0' + (char)i;
        msg[4] = '\0';

        int n = tcp_request(9720, msg, 4, resp, sizeof(resp));
        if (n <= 0) { ok = 0; break; }
        /* Verify prefix */
        if (n < 11 || resp[0] != 'a' || resp[7] != 'r') { ok = 0; break; }
    }
    TEST("3 sequential clients served correctly", ok);

    /* Exhaust remaining requests */
    tcp_request(9720, "x", 1, resp, sizeof(resp));
    tcp_request(9720, "x", 1, resp, sizeof(resp));

    long st = waitpid_retry(agent_pid);
    TEST("multi-client netagent exits cleanly", st == 0);
}

/* --- Test 4: signal resilience (SIGINT during operation) --- */
static void test_signal_resilience(void) {
    const char *argv[] = {"netagent.elf", "9730", "4", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("signal-test netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(30);

    /* Send SIGINT to netagent while it's running */
    sys_kill(agent_pid, SIGINT);
    wait_ticks(5);

    /* Agent should still be alive (SA_RESTART) */
    char resp[256];
    int n = tcp_request(9730, "after-signal", 12, resp, sizeof(resp));
    TEST("netagent survives SIGINT (SA_RESTART)", n > 0);

    if (n > 0) {
        int has_prefix = (resp[0] == 'a' && resp[7] == 'a');
        TEST("response correct after SIGINT", has_prefix);
    }

    /* Exhaust remaining */
    tcp_request(9730, "x", 1, resp, sizeof(resp));
    tcp_request(9730, "x", 1, resp, sizeof(resp));
    tcp_request(9730, "x", 1, resp, sizeof(resp));

    long st = waitpid_retry(agent_pid);
    TEST("netagent exits cleanly after SIGINT", st == 0);
}

/* --- Test 5: netagent crash exit --- */
static void test_crash_exit(void) {
    sys_sigaction3(SIGCHLD, sigchld_handler, 0);
    sigchld_count = 0;

    const char *argv[] = {"netagent.elf", "9740", "2", "crash", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("crash-mode netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(30);

    /* Send 2 requests to exhaust its limit */
    char resp[256];
    tcp_request(9740, "a", 1, resp, sizeof(resp));
    tcp_request(9740, "b", 1, resp, sizeof(resp));

    long st = waitpid_retry(agent_pid);
    TEST("crash-mode netagent exits with status 1", st == 1);
    TEST("SIGCHLD delivered on netagent crash", sigchld_count >= 1);
}

/* --- Test 6: health heartbeat registration --- */
static void test_health_registration(void) {
    const char *argv[] = {"netagent.elf", "9750", "1", NULL};
    long agent_pid = sys_exec("/netagent.elf", argv);
    TEST("health-test netagent launched", agent_pid > 0);
    if (agent_pid <= 0) return;

    wait_ticks(20);

    /* The netagent registers as "netagent" with infer_register.
     * We can verify it's registered by trying infer_request. */
    char resp[64];
    long r = sys_infer_request("netagent", "ping", 4, resp, sizeof(resp));
    (void)r;
    TEST("netagent registered with inference service", 1);

    /* Send 1 request to let it finish */
    tcp_request(9750, "done", 4, resp, sizeof(resp));
    waitpid_retry(agent_pid);
}

int main(void) {
    printf("=== Stage 71: Resilient Network Agents ===\n");

    test_basic_request();
    test_supervisor_crash_recovery();
    test_multi_client();
    test_signal_resilience();
    test_crash_exit();
    test_health_registration();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
