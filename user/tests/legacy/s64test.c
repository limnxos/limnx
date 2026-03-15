/* Stage 64 tests: TLS (arch_prctl), select(), supervisor trees */
#include "libc/libc.h"

static int pass = 0, fail = 0;
#define TEST(name, cond) do { \
    if (cond) { printf("  PASS: %s\n", name); pass++; } \
    else      { printf("  FAIL: %s\n", name); fail++; } \
} while(0)

/* ---- TLS tests ---- */

static void test_tls_set_get(void) {
    uint64_t val = 0xDEADBEEF12345678ULL;
    long r = sys_arch_prctl(ARCH_SET_FS, (long)val);
    TEST("arch_prctl SET_FS returns 0", r == 0);

    uint64_t got = 0;
    r = sys_arch_prctl(ARCH_GET_FS, (long)&got);
    TEST("arch_prctl GET_FS returns 0", r == 0);
    TEST("GET_FS returns correct value", got == val);
}

static void test_tls_fork_inherit(void) {
    uint64_t val = 0xCAFEBABE00000001ULL;
    sys_arch_prctl(ARCH_SET_FS, (long)val);

    long pid = sys_fork();
    if (pid == 0) {
        /* child */
        uint64_t got = 0;
        sys_arch_prctl(ARCH_GET_FS, (long)&got);
        if (got == val)
            sys_exit(42);
        else
            sys_exit(99);
    }
    long status = sys_waitpid(pid);
    TEST("fork inherits FS.base (TLS)", status == 42);
}

static void test_tls_zero(void) {
    sys_arch_prctl(ARCH_SET_FS, 0);
    uint64_t got = 0xFFFF;
    sys_arch_prctl(ARCH_GET_FS, (long)&got);
    TEST("FS.base can be set to 0", got == 0);
}

static void test_tls_invalid_code(void) {
    long r = sys_arch_prctl(0x9999, 0);
    TEST("invalid arch_prctl code returns -1", r == -1);
}

/* ---- select() tests ---- */

static void test_select_pipe_read(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    /* Write some data so read end is ready */
    sys_fwrite(wfd, "hello", 5);

    fd_set_t rfds;
    FD_ZERO(&rfds);
    FD_SET(rfd, &rfds);

    long ret = sys_select(rfd + 1, &rfds, NULL, 0);
    TEST("select detects pipe readable", ret > 0 && FD_ISSET(rfd, &rfds));

    sys_close(rfd);
    sys_close(wfd);
}

static void test_select_pipe_write(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    fd_set_t wfds;
    FD_ZERO(&wfds);
    FD_SET(wfd, &wfds);

    long ret = sys_select(wfd + 1, NULL, &wfds, 0);
    TEST("select detects pipe writable", ret > 0 && FD_ISSET(wfd, &wfds));

    sys_close(rfd);
    sys_close(wfd);
}

static void test_select_timeout(void) {
    long rfd, wfd;
    sys_pipe(&rfd, &wfd);

    fd_set_t rfds;
    FD_ZERO(&rfds);
    FD_SET(rfd, &rfds);

    /* Nothing written, should timeout immediately with timeout=0 */
    long ret = sys_select(rfd + 1, &rfds, NULL, 0);
    TEST("select returns 0 on timeout", ret == 0);

    sys_close(rfd);
    sys_close(wfd);
}

static void test_select_null_sets(void) {
    /* Both NULL = immediate return with 0 */
    long ret = sys_select(1, NULL, NULL, 0);
    TEST("select with NULL fd_sets returns 0", ret == 0);
}

static void test_select_multiple_fds(void) {
    long rfd1, wfd1, rfd2, wfd2;
    sys_pipe(&rfd1, &wfd1);
    sys_pipe(&rfd2, &wfd2);

    /* Write to pipe 2 only */
    sys_fwrite(wfd2, "data", 4);

    fd_set_t rfds;
    FD_ZERO(&rfds);
    FD_SET(rfd1, &rfds);
    FD_SET(rfd2, &rfds);

    long maxfd = rfd1 > rfd2 ? rfd1 : rfd2;
    long ret = sys_select(maxfd + 1, &rfds, NULL, 0);
    TEST("select multiple fds, only ready one set",
         ret > 0 && FD_ISSET(rfd2, &rfds) && !FD_ISSET(rfd1, &rfds));

    sys_close(rfd1); sys_close(wfd1);
    sys_close(rfd2); sys_close(wfd2);
}

/* ---- Supervisor tests ---- */

static void test_super_create(void) {
    long id = sys_super_create("test_sup");
    TEST("supervisor create returns valid id", id >= 0);
}

static void test_super_add_child(void) {
    long id = sys_super_create("sup_add");
    TEST("supervisor create for add test", id >= 0);

    long child = sys_super_add(id, "/hello.elf", -1, 0xFF);
    TEST("supervisor add child succeeds", child >= 0);
}

static void test_super_set_policy(void) {
    long id = sys_super_create("sup_pol");
    long r = sys_super_set_policy(id, SUPER_ONE_FOR_ALL);
    TEST("supervisor set policy ONE_FOR_ALL", r == 0);

    r = sys_super_set_policy(id, SUPER_ONE_FOR_ONE);
    TEST("supervisor set policy ONE_FOR_ONE", r == 0);
}

static void test_super_invalid(void) {
    long r = sys_super_set_policy(99, 0);
    TEST("supervisor invalid id returns -1", r == -1);

    long id = sys_super_create("sup_inv");
    r = sys_super_set_policy(id, 5);
    TEST("supervisor invalid policy returns -1", r == -1);
}

int main(void) {
    printf("=== Stage 64 Tests: TLS + select + supervisors ===\n");

    /* TLS tests */
    test_tls_set_get();
    test_tls_fork_inherit();
    test_tls_zero();
    test_tls_invalid_code();

    /* select() tests */
    test_select_pipe_read();
    test_select_pipe_write();
    test_select_timeout();
    test_select_null_sets();
    test_select_multiple_fds();

    /* Supervisor tests */
    test_super_create();
    test_super_add_child();
    test_super_set_policy();
    test_super_invalid();

    printf("=== Results: %d/%d passed ===\n", pass, pass + fail);
    if (fail == 0) printf("ALL TESTS PASSED\n");
    return fail;
}
