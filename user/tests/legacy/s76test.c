/* Stage 76 tests: Inference result caching
 *
 * Tests kernel-side response cache for inference services:
 *  - Cache stats baseline (empty)
 *  - Cache flush clears all entries
 *  - TTL configuration
 *  - Cache hit on repeated request (via provider)
 *  - Cache bypass when req_len == 0
 *  - Invalid cache control command
 *  - Cache miss counted correctly
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

/* --- Test 1: cache stats baseline --- */
static void test_stats_baseline(void) {
    /* Flush first to get clean state */
    long r = sys_infer_cache_ctrl(INFER_CACHE_FLUSH, (void *)0);
    TEST("cache flush succeeds", r == 0);

    infer_cache_stat_t stat;
    r = sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat);
    TEST("cache stats succeeds", r == 0);
    TEST("cache capacity is 32", stat.capacity == 32);
    TEST("cache size is 0 after flush", stat.size == 0);
    TEST("cache default TTL is 180", stat.ttl == 180);
}

/* --- Test 2: set TTL --- */
static void test_set_ttl(void) {
    long r = sys_infer_cache_ctrl(INFER_CACHE_SET_TTL, (void *)360);
    TEST("set TTL succeeds", r == 0);

    infer_cache_stat_t stat;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat);
    TEST("TTL updated to 360", stat.ttl == 360);

    /* Restore default */
    sys_infer_cache_ctrl(INFER_CACHE_SET_TTL, (void *)180);
}

/* --- Test 3: invalid command --- */
static void test_invalid_cmd(void) {
    long r = sys_infer_cache_ctrl(99, (void *)0);
    TEST("invalid cache cmd returns -EINVAL", r == -EINVAL);
}

/* --- Test 4: cache miss counted on request to nonexistent service --- */
static void test_cache_miss(void) {
    /* Flush to reset counters... actually counters persist, just check delta */
    infer_cache_stat_t stat_before;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_before);

    /* Request to nonexistent service — will miss cache, then fail route */
    /* Fork so the blocking queue doesn't hold us up */
    long pid = sys_fork();
    if (pid == 0) {
        char resp[64];
        /* This will queue and timeout, but the cache miss should be counted */
        sys_infer_request("cache_miss_svc", "query", 5, resp, 64);
        sys_exit(0);
    }
    waitpid_retry(pid);

    infer_cache_stat_t stat_after;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_after);
    TEST("cache miss counted", stat_after.misses > stat_before.misses);
}

/* --- Test 5: cache hit via inferd provider --- */
static void test_cache_hit(void) {
    /* Start inferd with service name "cachesvc" and unique socket path */
    const char *argv[] = {"inferd.elf", "cachesvc", "/tmp/cache1.sock", "10", NULL};
    long inferd_pid = sys_exec("/inferd.elf", argv);
    TEST("cache hit: inferd launched", inferd_pid > 0);
    if (inferd_pid <= 0) return;

    wait_ticks(50);

    /* Flush cache to start clean */
    sys_infer_cache_ctrl(INFER_CACHE_FLUSH, (void *)0);

    infer_cache_stat_t stat_before;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_before);

    /* First request — should be a cache miss, goes to provider */
    char resp1[256];
    long r1 = sys_infer_request("cachesvc", "test input", 10, resp1, 256);
    TEST("cache hit: first request succeeds", r1 > 0);

    infer_cache_stat_t stat_mid;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_mid);
    TEST("cache hit: first request was a miss",
         stat_mid.misses > stat_before.misses);
    TEST("cache hit: response cached (size > 0)", stat_mid.size > 0);

    /* Second identical request — should hit cache */
    char resp2[256];
    long r2 = sys_infer_request("cachesvc", "test input", 10, resp2, 256);
    TEST("cache hit: second request succeeds", r2 > 0);

    infer_cache_stat_t stat_after;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_after);
    TEST("cache hit: second request was a hit",
         stat_after.hits > stat_mid.hits);

    /* Responses should match */
    int match = (r1 == r2);
    if (match) {
        for (int i = 0; i < r1 && i < 256; i++) {
            if (resp1[i] != resp2[i]) { match = 0; break; }
        }
    }
    TEST("cache hit: cached response matches original", match);

    sys_kill(inferd_pid, 9);
    waitpid_retry(inferd_pid);
}

/* --- Test 6: cache bypass with req_len == 0 --- */
static void test_cache_bypass(void) {
    infer_cache_stat_t stat_before;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_before);

    /* Request with req_len=0 should bypass cache (no miss counted for cache) */
    /* Fork since it will queue/timeout */
    long pid = sys_fork();
    if (pid == 0) {
        char resp[64];
        sys_infer_request("bypass_svc", (void *)0, 0, resp, 64);
        sys_exit(0);
    }
    waitpid_retry(pid);

    infer_cache_stat_t stat_after;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat_after);
    /* Cache should not have been consulted (no miss increase) */
    TEST("cache bypass: no cache miss for req_len=0",
         stat_after.misses == stat_before.misses);
}

/* --- Test 7: flush clears cached entries --- */
static void test_flush_clears(void) {
    /* Start inferd to populate cache */
    const char *argv[] = {"inferd.elf", "flushsvc", "/tmp/flush1.sock", "10", NULL};
    long inferd_pid = sys_exec("/inferd.elf", argv);
    if (inferd_pid <= 0) {
        TEST("flush: inferd launched", 0);
        return;
    }

    wait_ticks(50);

    /* Make a request to populate cache */
    char resp[256];
    sys_infer_request("flushsvc", "flush test", 10, resp, 256);

    infer_cache_stat_t stat;
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat);
    TEST("flush: cache has entries before flush", stat.size > 0);

    /* Flush */
    sys_infer_cache_ctrl(INFER_CACHE_FLUSH, (void *)0);
    sys_infer_cache_ctrl(INFER_CACHE_STATS, &stat);
    TEST("flush: cache empty after flush", stat.size == 0);

    sys_kill(inferd_pid, 9);
    waitpid_retry(inferd_pid);
}

int main(void) {
    printf("=== Stage 76: Inference Result Caching ===\n");

    test_stats_baseline();
    test_set_ttl();
    test_invalid_cmd();
    test_cache_miss();
    test_cache_hit();
    test_cache_bypass();
    test_flush_clears();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
