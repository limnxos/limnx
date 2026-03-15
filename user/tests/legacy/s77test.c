/* Stage 77 tests: Agent-to-agent inference delegation
 *
 * Tests capability token integration with inference services:
 *  - Process with CAP_INFER can register and request
 *  - Process without CAP_INFER is denied
 *  - Token delegation grants scoped inference access
 *  - Token revocation removes access
 *  - Service-scoped tokens only work for matching service
 *  - Bearer tokens (target_pid=0) work for any process
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

/* --- Test 1: process with CAP_INFER can register --- */
static void test_cap_infer_register(void) {
    /* We have CAP_ALL which includes CAP_INFER */
    long r = sys_infer_register("test77svc", "/tmp/test77.sock");
    TEST("CAP_INFER: register succeeds", r == 0);
}

/* --- Test 2: process without CAP_INFER is denied register --- */
static void test_no_cap_register(void) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Drop all caps except basic ones (no CAP_INFER) */
        sys_setcap(sys_getpid(), CAP_FS_READ | CAP_FS_WRITE);

        long r = sys_infer_register("denied_svc", "/tmp/denied.sock");
        sys_exit(r == -EACCES ? 0 : 1);
    }
    long st = waitpid_retry(pid);
    TEST("no CAP_INFER: register denied", st == 0);
}

/* --- Test 3: process without CAP_INFER is denied request --- */
static void test_no_cap_request(void) {
    long pid = sys_fork();
    if (pid == 0) {
        /* Drop caps */
        sys_setcap(sys_getpid(), CAP_FS_READ | CAP_FS_WRITE);

        char resp[64];
        long r = sys_infer_request("any_svc", "hello", 5, resp, 64);
        sys_exit(r == -EACCES ? 0 : 1);
    }
    long st = waitpid_retry(pid);
    TEST("no CAP_INFER: request denied", st == 0);
}

/* --- Test 4: token grants inference access to child --- */
static void test_token_grants_access(void) {
    /* Create a provider first (we have CAP_ALL) */
    long provider = sys_fork();
    if (provider == 0) {
        sys_infer_register("tokensvc", "/tmp/token.sock");
        sys_infer_health(0);
        for (int i = 0; i < 500; i++) {
            sys_infer_health(0);
            sys_yield();
        }
        sys_exit(0);
    }
    wait_ticks(20);

    /* Fork a child, drop caps, but give it a token */
    long child = sys_fork();
    if (child == 0) {
        long my_pid = sys_getpid();

        /* Drop all caps */
        sys_setcap(my_pid, 0);

        /* Without token, request should fail */
        char resp[64];
        long r1 = sys_infer_request("tokensvc", "q1", 2, resp, 64);
        if (r1 != -EACCES) sys_exit(10);

        /* Signal parent to create token for us */
        /* Just yield and let parent create token */
        wait_ticks(50);

        /* Now try again — parent should have created token */
        long r2 = sys_infer_request("tokensvc", "q2", 2, resp, 64);
        /* Should not be -EACCES (might be -ECONNREFUSED since provider
         * isn't a real unix socket server, but that's fine — not denied) */
        sys_exit(r2 == -EACCES ? 1 : 0);
    }

    wait_ticks(10);

    /* Create token granting CAP_INFER to child, scoped to "tokensvc" */
    long token_id = sys_token_create(CAP_INFER, child, "tokensvc");
    TEST("token create for child", token_id > 0);

    long st = waitpid_retry(child);
    TEST("token grants inference access", st == 0);

    /* Clean up */
    if (token_id > 0) sys_token_revoke(token_id);
    sys_kill(provider, 9);
    waitpid_retry(provider);
}

/* --- Test 5: token revocation removes access --- */
static void test_token_revocation(void) {
    long child = sys_fork();
    if (child == 0) {
        /* Drop caps */
        sys_setcap(sys_getpid(), 0);

        /* Wait for parent to create and then revoke token */
        wait_ticks(50);

        /* Token should be revoked by now */
        char resp[64];
        long r = sys_infer_request("revoke_svc", "q", 1, resp, 64);
        sys_exit(r == -EACCES ? 0 : 1);
    }

    /* Create token, then immediately revoke it */
    long token_id = sys_token_create(CAP_INFER, child, "revoke_svc");
    TEST("revocation: token created", token_id > 0);

    if (token_id > 0) {
        long r = sys_token_revoke(token_id);
        TEST("revocation: token revoked", r == 0);
    }

    long st = waitpid_retry(child);
    TEST("revocation: access denied after revoke", st == 0);
}

/* --- Test 6: service-scoped token only works for matching service --- */
static void test_service_scoping(void) {
    long child = sys_fork();
    if (child == 0) {
        sys_setcap(sys_getpid(), 0);

        wait_ticks(30);

        /* Token is scoped to "allowed_svc" */
        char resp[64];
        long r1 = sys_infer_request("allowed_svc", "q", 1, resp, 64);
        int allowed_ok = (r1 != -EACCES);

        long r2 = sys_infer_request("other_svc", "q", 1, resp, 64);
        int other_denied = (r2 == -EACCES);

        sys_exit((allowed_ok && other_denied) ? 0 : 1);
    }

    /* Create token scoped to "allowed_svc" */
    long token_id = sys_token_create(CAP_INFER, child, "allowed_svc");
    TEST("scoping: token created for allowed_svc", token_id > 0);

    long st = waitpid_retry(child);
    TEST("scoping: allowed_svc ok, other_svc denied", st == 0);

    if (token_id > 0) sys_token_revoke(token_id);
}

/* --- Test 7: bearer token (target_pid=0) works for any process --- */
static void test_bearer_token(void) {
    /* Create bearer token for CAP_INFER scoped to "bearer_svc" */
    long token_id = sys_token_create(CAP_INFER, 0, "bearer_svc");
    TEST("bearer: token created", token_id > 0);

    long child = sys_fork();
    if (child == 0) {
        sys_setcap(sys_getpid(), 0);

        char resp[64];
        long r = sys_infer_request("bearer_svc", "q", 1, resp, 64);
        /* Should not be -EACCES (bearer token covers any pid) */
        sys_exit(r == -EACCES ? 1 : 0);
    }

    long st = waitpid_retry(child);
    TEST("bearer: any process can use bearer token", st == 0);

    if (token_id > 0) sys_token_revoke(token_id);
}

/* --- Test 8: delegation chain --- */
static void test_delegation(void) {
    /* Use a pipe to pass token ID from parent to child A */
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    long child_a = sys_fork();
    if (child_a == 0) {
        sys_close(wfd);

        /* Read token ID from parent via pipe */
        long token_id_buf = 0;
        sys_read(rfd, &token_id_buf, sizeof(long));
        sys_close(rfd);

        if (token_id_buf <= 0) sys_exit(2);

        /* Fork child B */
        long child_b = sys_fork();
        if (child_b == 0) {
            sys_setcap(sys_getpid(), 0);
            wait_ticks(30);

            char resp[64];
            long r = sys_infer_request("deleg_svc", "q", 1, resp, 64);
            sys_exit(r == -EACCES ? 1 : 0);
        }

        /* Delegate our token to child B */
        sys_token_delegate(token_id_buf, child_b, CAP_INFER, "deleg_svc");

        long st = waitpid_retry(child_b);
        sys_exit(st == 0 ? 0 : 1);
    }

    sys_close(rfd);

    /* Create token for child A */
    long token_id = sys_token_create(CAP_INFER, child_a, "deleg_svc");
    TEST("delegation: parent token created", token_id > 0);

    /* Send token ID to child A */
    sys_fwrite(wfd, &token_id, sizeof(long));
    sys_close(wfd);

    long st = waitpid_retry(child_a);
    TEST("delegation: chain A→B grants access", st == 0);

    if (token_id > 0) sys_token_revoke(token_id);
}

/* --- Test 9: process with CAP_INFER can request directly --- */
static void test_cap_infer_request(void) {
    /* We have CAP_ALL including CAP_INFER — should not get -EACCES */
    char resp[64];
    long r = sys_infer_request("direct_cap", "q", 1, resp, 64);
    /* Will fail with -EAGAIN (no provider, queues and times out) or similar,
     * but NOT -EACCES */
    TEST("CAP_INFER: request not denied (not -EACCES)", r != -EACCES);
}

int main(void) {
    printf("=== Stage 77: Agent-to-Agent Inference Delegation ===\n");

    test_cap_infer_register();
    test_no_cap_register();
    test_no_cap_request();
    test_token_grants_access();
    test_token_revocation();
    test_service_scoping();
    test_bearer_token();
    test_delegation();
    test_cap_infer_request();

    printf("--- Results: %d passed, %d failed ---\n", pass, fail);
    return fail;
}
