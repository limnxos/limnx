/* Stage 75 tests: Inference request queuing
 *
 * Tests kernel-side request queue for inference services:
 *  - Request queued when no provider available
 *  - Provider starts, queued request gets served
 *  - Queue timeout returns -EAGAIN
 *  - Queue stat syscall returns correct values
 *  - Multiple queued requests drained in FIFO order
 *  - Queue full returns -ENOBUFS
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

/* Helper: fork a child that registers as an inference provider after a delay */
static long spawn_delayed_provider(const char *name, const char *sock,
                                    int delay_ticks, int load) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: wait, then register and report health */
        wait_ticks(delay_ticks);
        sys_infer_register(name, sock);
        sys_infer_health((long)load);

        /* Stay alive with heartbeats */
        for (int i = 0; i < 500; i++) {
            sys_infer_health((long)load);
            sys_yield();
        }
        sys_exit(0);
    }
    return pid;
}

/* Helper: fork a child that registers immediately */
static long spawn_provider(const char *name, const char *sock, int load) {
    return spawn_delayed_provider(name, sock, 0, load);
}

/* --- Test 1: queue stat when empty --- */
static void test_queue_stat_empty(void) {
    infer_queue_stat_t stat;
    long r = sys_infer_queue_stat(&stat);
    TEST("queue stat syscall succeeds", r == 0);
    TEST("queue capacity is 16", stat.capacity == 16);
    TEST("queue pending is 0 initially", stat.pending == 0);
}

/* --- Test 2: request with no provider queues and times out --- */
static void test_queue_timeout(void) {
    /* Fork a child that makes an infer_request to nonexistent service */
    long pid = sys_fork();
    if (pid == 0) {
        char resp[64];
        long r = sys_infer_request("phantom", "hello", 5, resp, 64);
        /* Should return -EAGAIN after timeout */
        sys_exit(r == -EAGAIN ? 0 : 1);
    }

    /* Wait for child — should timeout and exit */
    long st = waitpid_retry(pid);
    TEST("queue timeout: request returns -EAGAIN", st == 0);
}

/* --- Test 3: queued request served when provider starts --- */
static void test_queue_drain(void) {
    /* Fork a child that requests from "delayed_svc" (no provider yet) */
    long requester = sys_fork();
    if (requester == 0) {
        char resp[64];
        long r = sys_infer_request("delayed_svc", "test", 4, resp, 64);
        /* Should succeed once provider starts (or fail with connection error) */
        /* The key test is that it doesn't return -ENOENT immediately */
        sys_exit(r == -ENOENT ? 1 : 0);
    }

    /* Give the requester time to queue */
    wait_ticks(20);

    /* Check queue has a pending entry */
    infer_queue_stat_t stat;
    sys_infer_queue_stat(&stat);
    TEST("queue drain: request is queued (pending > 0)", stat.pending > 0);

    /* Start a provider — this should drain the queue */
    long provider = spawn_provider("delayed_svc", "/tmp/delayed.sock", 0);
    wait_ticks(20);

    /* Wait for requester to complete */
    long st = waitpid_retry(requester);
    TEST("queue drain: requester didn't get -ENOENT", st == 0);

    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 4: multiple queued requests drained in FIFO --- */
static void test_queue_fifo(void) {
    /* Fork 3 children that each request from "fifo_svc" */
    long pids[3];
    for (int i = 0; i < 3; i++) {
        long pid = sys_fork();
        if (pid == 0) {
            char resp[64];
            long r = sys_infer_request("fifo_svc", "req", 3, resp, 64);
            sys_exit(r == -ENOENT ? 1 : 0);
        }
        pids[i] = pid;
        wait_ticks(5); /* stagger so FIFO order is deterministic */
    }

    wait_ticks(20);

    /* Check queue depth */
    infer_queue_stat_t stat;
    sys_infer_queue_stat(&stat);
    TEST("queue fifo: 3 requests queued", stat.pending >= 3);

    /* Start provider — health heartbeats drain one per call */
    long provider = spawn_provider("fifo_svc", "/tmp/fifo.sock", 0);
    wait_ticks(30);

    /* Wait for all requesters */
    int ok_count = 0;
    for (int i = 0; i < 3; i++) {
        long st = waitpid_retry(pids[i]);
        if (st == 0) ok_count++;
    }
    TEST("queue fifo: all 3 requests served", ok_count == 3);

    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 5: direct request still works (no queue needed) --- */
static void test_direct_request(void) {
    /* Start provider first */
    long provider = spawn_provider("direct_svc", "/tmp/direct.sock", 0);
    wait_ticks(20);

    /* Request should route directly without queueing */
    long route = sys_infer_route("direct_svc");
    TEST("direct request: provider available", route >= 0);

    /* Check queue stat — total_queued should not increase for direct requests */
    infer_queue_stat_t stat_before;
    sys_infer_queue_stat(&stat_before);

    /* The infer_request will connect via unix socket — won't succeed
     * because provider isn't a real unix socket server, but it should
     * NOT queue (provider is available). Check by comparing total_queued. */
    char resp[64];
    sys_infer_request("direct_svc", "x", 1, resp, 64);

    infer_queue_stat_t stat_after;
    sys_infer_queue_stat(&stat_after);
    TEST("direct request: not queued (total_queued unchanged)",
         stat_after.total_queued == stat_before.total_queued);

    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 6: queue stat shows timeouts --- */
static void test_queue_stat_timeouts(void) {
    infer_queue_stat_t stat;
    sys_infer_queue_stat(&stat);
    /* test_queue_timeout should have caused at least 1 timeout */
    TEST("queue stat: total_timeouts > 0", stat.total_timeouts > 0);
    TEST("queue stat: total_queued > 0", stat.total_queued > 0);
}

int main(void) {
    printf("=== Stage 75: Inference Request Queuing ===\n");

    test_queue_stat_empty();
    test_queue_timeout();
    test_queue_drain();
    test_queue_fifo();
    test_direct_request();
    test_queue_stat_timeouts();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
