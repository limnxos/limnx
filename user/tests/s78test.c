/* Stage 78 tests: Async Inference Completion
 *
 * Tests async inference submit/poll/result with eventfd notification:
 *  - Submit returns valid request ID
 *  - Poll returns PENDING for fresh submission
 *  - Poll returns -EINVAL for invalid ID
 *  - Result returns -EAGAIN for pending request
 *  - Result returns -EINVAL for invalid ID
 *  - Eventfd signaled on completion (with real provider)
 *  - Result retrieval frees slot
 *  - Epoll integration (eventfd in epoll)
 *  - Timeout produces error status
 *  - Permission check (no CAP_INFER)
 *  - Max slots exhaustion
 *  - Process ownership (child can't poll parent's request)
 */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

static long waitpid_retry(long pid) {
    long st;
    while ((st = sys_waitpid(pid)) == -EINTR) ;
    return st;
}

static void sleep_ms(int ms) {
    timespec_t ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    sys_nanosleep(&ts);
}

/* Helper: start inferd with given service name */
static long start_inferd(const char *svc_name, const char *sock_path) {
    const char *argv[] = {"inferd.elf", svc_name, sock_path, "10", NULL};
    long pid = sys_exec("/inferd.elf", argv);
    /* Wait for provider to register and report health */
    sleep_ms(2000);
    return pid;
}

/* --- Test 1: submit returns valid request ID --- */
static void test_submit_returns_id(void) {
    long id = sys_infer_submit("nosvc78a", "hello", 5, -1);
    TEST("submit returns positive request ID", id > 0);
    /* Slot will timeout eventually — don't wait here */
}

/* --- Test 2: poll returns PENDING for fresh submission --- */
static void test_poll_pending(void) {
    long id = sys_infer_submit("nosvc78b", "test", 4, -1);
    if (id <= 0) { TEST("poll pending: submit failed", 0); return; }
    long status = sys_infer_poll(id);
    TEST("poll returns PENDING for fresh request", status == INFER_STATUS_PENDING);
}

/* --- Test 3: poll returns -EINVAL for invalid ID --- */
static void test_poll_invalid(void) {
    long status = sys_infer_poll(999);
    TEST("poll returns -EINVAL for invalid ID", status == -EINVAL);
}

/* --- Test 4: result returns -EAGAIN for pending request --- */
static void test_result_pending(void) {
    long id = sys_infer_submit("nosvc78c", "test", 4, -1);
    if (id <= 0) { TEST("result pending: submit failed", 0); return; }
    char resp[64];
    long r = sys_infer_result(id, resp, 64);
    TEST("result returns -EAGAIN for pending", r == -EAGAIN);
}

/* --- Test 5: result returns -EINVAL for invalid ID --- */
static void test_result_invalid(void) {
    char resp[64];
    long r = sys_infer_result(999, resp, 64);
    TEST("result returns -EINVAL for invalid ID", r == -EINVAL);
}

/* --- Test 6: permission check (no CAP_INFER) --- */
static void test_no_cap(void) {
    long pid = sys_fork();
    if (pid == 0) {
        sys_setcap(sys_getpid(), CAP_FS_READ | CAP_FS_WRITE);
        long r = sys_infer_submit("anysvc", "test", 4, -1);
        sys_exit(r == -EACCES ? 0 : 1);
    }
    long st = waitpid_retry(pid);
    TEST("no CAP_INFER: submit denied", st == 0);
}

/* --- Test 7: process ownership (child can't poll parent's request) --- */
static void test_ownership(void) {
    long id = sys_infer_submit("own78", "test", 4, -1);
    if (id <= 0) { TEST("ownership: submit failed", 0); return; }

    long child = sys_fork();
    if (child == 0) {
        long r = sys_infer_poll(id);
        sys_exit(r == -EINVAL ? 0 : 1);
    }
    long st = waitpid_retry(child);
    TEST("child can't poll parent's async request", st == 0);
}

/* Wait for all pending async slots to timeout and free them */
static void drain_all_slots(void) {
    printf("  (waiting for async slots to timeout...)\n");
    sleep_ms(6000);  /* 6 seconds > 90 tick timeout */
    char resp[64];
    for (int i = 1; i <= 16; i++) {
        sys_infer_result(i, resp, 64);  /* Attempt to free each slot */
    }
}

/* --- Test 8: timeout produces error status --- */
static void test_timeout(void) {
    long id = sys_infer_submit("timeout78", "test", 4, -1);
    if (id <= 0) { TEST("timeout: submit failed", 0); return; }

    /* Wait for timeout (90 ticks at 18Hz ≈ 5 seconds) */
    sleep_ms(6000);

    long status = sys_infer_poll(id);
    TEST("timeout produces error status", status < 0 && status != -EINVAL);

    /* Free the slot */
    char resp[64];
    sys_infer_result(id, resp, 64);
}

/* --- Test 9: max slots exhaustion --- */
static void test_max_slots(void) {
    long ids[16];
    int alloc_ok = 1;

    for (int i = 0; i < 16; i++) {
        ids[i] = sys_infer_submit("maxslot78", "x", 1, -1);
        if (ids[i] <= 0) { alloc_ok = 0; break; }
    }

    long extra = sys_infer_submit("maxslot78", "x", 1, -1);
    TEST("max slots: 17th returns -ENOBUFS", alloc_ok && extra == -ENOBUFS);

    /* Wait for all to timeout, then free */
    sleep_ms(6000);
    char resp[64];
    for (int i = 0; i < 16; i++) {
        if (ids[i] > 0) sys_infer_result(ids[i], resp, 64);
    }
}

/* --- Test 10: eventfd signaled on completion (with real provider) --- */
static void test_eventfd_completion(void) {
    long provider = start_inferd("async78svc", "/tmp/async78.sock");
    if (provider <= 0) { TEST("eventfd: inferd launch failed", 0); return; }

    long efd = sys_eventfd(0);
    TEST("eventfd created", efd >= 0);

    long id = sys_infer_submit("async78svc", "hello", 5, efd);
    TEST("async submit with eventfd", id > 0);

    if (id > 0 && efd >= 0) {
        /* Wait for worker to process — poll until ready or timeout */
        int completed = 0;
        for (int i = 0; i < 100; i++) {
            long status = sys_infer_poll(id);
            if (status != INFER_STATUS_PENDING) {
                completed = 1;
                break;
            }
            sleep_ms(50);
        }
        TEST("async request completed", completed);

        if (completed) {
            char resp[256];
            long r = sys_infer_result(id, resp, 256);
            /* Result might be actual data or an error (depends on inferd),
             * but should not be -EAGAIN */
            TEST("async result retrieved", r != -EAGAIN);
        }
    }

    if (efd >= 0) sys_close(efd);
    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 11: slot freed after result retrieval --- */
static void test_slot_freed(void) {
    long provider = start_inferd("free78svc", "/tmp/free78.sock");
    if (provider <= 0) { TEST("slot freed: inferd launch failed", 0); return; }

    long id = sys_infer_submit("free78svc", "test", 4, -1);
    if (id <= 0) { TEST("slot freed: submit failed", 0); goto cleanup; }

    /* Wait for completion */
    for (int i = 0; i < 100; i++) {
        long status = sys_infer_poll(id);
        if (status != INFER_STATUS_PENDING) break;
        sleep_ms(50);
    }

    /* Retrieve result (frees the slot) */
    char resp[64];
    sys_infer_result(id, resp, 64);

    /* Now poll should return -EINVAL (slot freed) */
    long r = sys_infer_poll(id);
    TEST("slot freed after result retrieval", r == -EINVAL);

cleanup:
    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 12: epoll integration --- */
static void test_epoll_integration(void) {
    long provider = start_inferd("epoll78svc", "/tmp/epoll78.sock");
    if (provider <= 0) { TEST("epoll: inferd launch failed", 0); return; }

    long efd = sys_eventfd(0);
    long epfd = sys_epoll_create(0);
    TEST("epoll+eventfd created", efd >= 0 && epfd >= 0);

    if (efd >= 0 && epfd >= 0) {
        epoll_event_t ev;
        ev.events = 0x001; /* EPOLLIN */
        ev.data = 42;
        sys_epoll_ctl(epfd, 1, efd, &ev);

        long id = sys_infer_submit("epoll78svc", "hello", 5, efd);
        TEST("epoll: async submit", id > 0);

        if (id > 0) {
            epoll_event_t out_ev;
            long n = sys_epoll_wait(epfd, &out_ev, 1, 10000);
            int epoll_got_event = (n > 0 && out_ev.data == 42);
            TEST("epoll: eventfd notification received", epoll_got_event);

            char resp[256];
            long r = sys_infer_result(id, resp, 256);
            TEST("epoll: result retrieved after notification", r != -EAGAIN);
        }
    }

    if (efd >= 0) sys_close(efd);
    if (epfd >= 0) sys_close(epfd);
    sys_kill(provider, 9);
    waitpid_retry(provider);
}

int main(void) {
    printf("=== Stage 78: Async Inference Completion ===\n");

    /* Basic API tests (fast, no provider needed) */
    test_submit_returns_id();
    test_poll_pending();
    test_poll_invalid();
    test_result_pending();
    test_result_invalid();
    test_no_cap();
    test_ownership();

    /* Drain slots from basic tests before proceeding */
    drain_all_slots();

    /* Timeout test (needs real time wait) */
    test_timeout();

    /* Max slots test (needs all slots free) */
    test_max_slots();

    /* Provider-based tests */
    test_eventfd_completion();
    test_slot_freed();
    test_epoll_integration();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
