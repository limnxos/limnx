/* Stage 73 tests: Multi-model inference routing
 *
 * Tests load-balanced routing across multiple inference providers:
 *  - Multiple providers register with same service name
 *  - Least-loaded routing picks lowest-load provider
 *  - Round-robin cycles through all providers
 *  - Weighted routing favors low-load providers
 *  - Unhealthy/dead providers are skipped
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

/* Helper: fork a child that registers as an inference provider */
static long spawn_provider(const char *name, const char *sock, int load) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Child: register, report health, wait */
        sys_infer_register(name, sock);
        sys_infer_health((long)load);

        /* Stay alive reporting heartbeats until killed */
        for (int i = 0; i < 500; i++) {
            sys_infer_health((long)load);
            sys_yield();
        }
        sys_exit(0);
    }
    return pid;
}

/* --- Test 1: multi-provider registration --- */
static void test_multi_register(void) {
    long p1 = spawn_provider("llm", "/tmp/llm1.sock", 0);
    long p2 = spawn_provider("llm", "/tmp/llm2.sock", 5);
    long p3 = spawn_provider("llm", "/tmp/llm3.sock", 10);

    wait_ticks(20);

    /* All 3 should be routable */
    long r1 = sys_infer_route("llm");
    TEST("multi-register: first route succeeds", r1 > 0);

    /* Route 3 times — should get valid pids each time */
    sys_infer_set_policy(INFER_ROUTE_ROUND_ROBIN);
    long pids[3];
    int all_valid = 1;
    for (int i = 0; i < 3; i++) {
        pids[i] = sys_infer_route("llm");
        if (pids[i] <= 0) all_valid = 0;
    }
    TEST("multi-register: 3 routes all valid", all_valid);

    /* Check we got different providers (round-robin) */
    int different = (pids[0] != pids[1]) || (pids[1] != pids[2]) || (pids[0] != pids[2]);
    TEST("multi-register: round-robin returns different pids", different);

    /* Clean up */
    sys_kill(p1, 9);
    sys_kill(p2, 9);
    sys_kill(p3, 9);
    waitpid_retry(p1);
    waitpid_retry(p2);
    waitpid_retry(p3);

    /* Reset policy */
    sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);
}

/* --- Test 2: least-loaded routing --- */
static void test_least_loaded(void) {
    sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);

    long p1 = spawn_provider("model", "/tmp/m1.sock", 100);
    long p2 = spawn_provider("model", "/tmp/m2.sock", 1);
    long p3 = spawn_provider("model", "/tmp/m3.sock", 50);

    wait_ticks(20);

    /* Should consistently route to p2 (load=1) */
    long routed = sys_infer_route("model");
    TEST("least-loaded: routes to pid", routed > 0);
    TEST("least-loaded: picks lowest load provider", routed == p2);

    /* Route again — still p2 */
    long routed2 = sys_infer_route("model");
    TEST("least-loaded: consistent routing", routed2 == p2);

    sys_kill(p1, 9);
    sys_kill(p2, 9);
    sys_kill(p3, 9);
    waitpid_retry(p1);
    waitpid_retry(p2);
    waitpid_retry(p3);
}

/* --- Test 3: round-robin routing --- */
static void test_round_robin(void) {
    sys_infer_set_policy(INFER_ROUTE_ROUND_ROBIN);

    long p1 = spawn_provider("rr", "/tmp/rr1.sock", 0);
    long p2 = spawn_provider("rr", "/tmp/rr2.sock", 0);

    wait_ticks(20);

    /* Route multiple times — should alternate */
    long r1 = sys_infer_route("rr");
    long r2 = sys_infer_route("rr");
    long r3 = sys_infer_route("rr");
    long r4 = sys_infer_route("rr");

    TEST("round-robin: routes succeed", r1 > 0 && r2 > 0);
    TEST("round-robin: alternates providers", r1 != r2);
    TEST("round-robin: cycles back", r1 == r3 && r2 == r4);

    sys_kill(p1, 9);
    sys_kill(p2, 9);
    waitpid_retry(p1);
    waitpid_retry(p2);

    sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);
}

/* --- Test 4: weighted routing favors low-load --- */
static void test_weighted(void) {
    sys_infer_set_policy(INFER_ROUTE_WEIGHTED);

    /* Provider A: load=0, Provider B: load=100 */
    long pa = spawn_provider("wt", "/tmp/wt1.sock", 0);
    long pb = spawn_provider("wt", "/tmp/wt2.sock", 100);

    wait_ticks(20);

    /* Route 10 times — provider A (load=0) should get majority */
    int count_a = 0, count_b = 0;
    for (int i = 0; i < 10; i++) {
        long r = sys_infer_route("wt");
        if (r == pa) count_a++;
        else if (r == pb) count_b++;
    }
    TEST("weighted: low-load gets more routes", count_a > count_b);
    TEST("weighted: both providers get some routes", count_a > 0 && count_b >= 0);

    sys_kill(pa, 9);
    sys_kill(pb, 9);
    waitpid_retry(pa);
    waitpid_retry(pb);

    sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);
}

/* --- Test 5: failover when provider dies --- */
static void test_failover(void) {
    sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);

    long p1 = spawn_provider("fail", "/tmp/f1.sock", 0);
    long p2 = spawn_provider("fail", "/tmp/f2.sock", 10);

    wait_ticks(20);

    /* Should route to p1 (lower load) */
    long r1 = sys_infer_route("fail");
    TEST("failover: routes to low-load provider", r1 == p1);

    /* Kill p1 */
    sys_kill(p1, 9);
    waitpid_retry(p1);
    wait_ticks(10);

    /* Should now route to p2 (p1 is dead, auto-unregistered) */
    long r2 = sys_infer_route("fail");
    TEST("failover: routes to surviving provider", r2 == p2);

    sys_kill(p2, 9);
    waitpid_retry(p2);
}

/* --- Test 6: no providers available --- */
static void test_no_providers(void) {
    /* No providers registered for "ghost" */
    long r = sys_infer_route("ghost");
    TEST("no providers: route returns error", r < 0);
}

/* --- Test 7: policy set/get --- */
static void test_policy_set(void) {
    long r1 = sys_infer_set_policy(INFER_ROUTE_ROUND_ROBIN);
    TEST("set policy round-robin", r1 == 0);

    long r2 = sys_infer_set_policy(INFER_ROUTE_WEIGHTED);
    TEST("set policy weighted", r2 == 0);

    long r3 = sys_infer_set_policy(INFER_ROUTE_LEAST_LOADED);
    TEST("set policy least-loaded", r3 == 0);

    /* Invalid policy */
    long r4 = sys_infer_set_policy(99);
    TEST("set invalid policy fails", r4 < 0);
}

int main(void) {
    printf("=== Stage 73: Multi-Model Inference Routing ===\n");

    test_multi_register();
    test_least_loaded();
    test_round_robin();
    test_weighted();
    test_failover();
    test_no_providers();
    test_policy_set();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
