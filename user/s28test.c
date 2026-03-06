#include "libc/libc.h"

/* --- Test framework --- */

static int tests_passed = 0;
static int tests_total  = 0;

static void pass(const char *name) {
    tests_passed++;
    tests_total++;
    printf("  [PASS] %s\n", name);
}

static void fail(const char *name, const char *reason) {
    tests_total++;
    printf("  [FAIL] %s — %s\n", name, reason);
}

/* --- Test 1: argc/argv from boot --- */
static void test_argv(int argc, char **argv) {
    const char *name = "argc/argv from boot";

    if (argc < 2) {
        fail(name, "argc < 2");
        return;
    }
    if (strcmp(argv[1], "--test") != 0) {
        fail(name, "argv[1] != --test");
        return;
    }
    pass(name);
}

/* --- Test 2: fcntl F_SETFD/F_GETFD --- */
static void test_fcntl_cloexec(void) {
    const char *name = "fcntl F_SETFD/F_GETFD";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    /* Initially cloexec should be 0 */
    long val = sys_fcntl(wfd, F_GETFD, 0);
    if (val != 0) { fail(name, "initial cloexec not 0"); goto out; }

    /* Set cloexec */
    if (sys_fcntl(wfd, F_SETFD, FD_CLOEXEC) != 0) { fail(name, "setfd failed"); goto out; }

    /* Read it back */
    val = sys_fcntl(wfd, F_GETFD, 0);
    if (val != FD_CLOEXEC) { fail(name, "cloexec not set"); goto out; }

    /* Clear it */
    if (sys_fcntl(wfd, F_SETFD, 0) != 0) { fail(name, "clear failed"); goto out; }
    val = sys_fcntl(wfd, F_GETFD, 0);
    if (val != 0) { fail(name, "cloexec not cleared"); goto out; }

    pass(name);
out:
    sys_close(rfd);
    sys_close(wfd);
}

/* --- Test 3: fcntl F_SETFL/F_GETFL --- */
static void test_fcntl_nonblock(void) {
    const char *name = "fcntl F_SETFL/F_GETFL";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    /* Initially nonblock should be 0 */
    long val = sys_fcntl(rfd, F_GETFL, 0);
    if (val != 0) { fail(name, "initial nonblock not 0"); goto out; }

    /* Set nonblock */
    if (sys_fcntl(rfd, F_SETFL, O_NONBLOCK) != 0) { fail(name, "setfl failed"); goto out; }

    /* Read it back */
    val = sys_fcntl(rfd, F_GETFL, 0);
    if (!(val & O_NONBLOCK)) { fail(name, "nonblock not set"); goto out; }

    /* Clear it */
    if (sys_fcntl(rfd, F_SETFL, 0) != 0) { fail(name, "clear failed"); goto out; }
    val = sys_fcntl(rfd, F_GETFL, 0);
    if (val & O_NONBLOCK) { fail(name, "nonblock not cleared"); goto out; }

    pass(name);
out:
    sys_close(rfd);
    sys_close(wfd);
}

/* --- Test 4: O_NONBLOCK pipe read --- */
static void test_nonblock_read(void) {
    const char *name = "O_NONBLOCK pipe read";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    /* Set read end to nonblock */
    sys_fcntl(rfd, F_SETFL, O_NONBLOCK);

    /* Read with no data should return -1 immediately */
    char buf[16];
    long n = sys_read(rfd, buf, sizeof(buf));
    if (n != -1) {
        printf("    got n=%ld, expected -1\n", n);
        fail(name, "nonblock read did not return -1");
    } else {
        pass(name);
    }

    sys_close(rfd);
    sys_close(wfd);
}

/* --- Test 5: O_NONBLOCK pipe write --- */
static void test_nonblock_write(void) {
    const char *name = "O_NONBLOCK pipe write";

    long rfd, wfd;
    if (sys_pipe(&rfd, &wfd) != 0) { fail(name, "pipe failed"); return; }

    /* Set write end to nonblock */
    sys_fcntl(wfd, F_SETFL, O_NONBLOCK);

    /* Fill the pipe (4096 bytes) */
    char fill[256];
    memset(fill, 'A', sizeof(fill));
    int total = 0;
    for (int i = 0; i < 20; i++) {
        long n = sys_fwrite(wfd, fill, sizeof(fill));
        if (n <= 0) break;
        total += (int)n;
    }

    /* Now pipe should be full, next write should return -1 */
    long n = sys_fwrite(wfd, "X", 1);
    if (n == -1) {
        pass(name);
    } else {
        printf("    total filled=%d, extra write returned %ld\n", total, n);
        fail(name, "nonblock write did not return -1 on full pipe");
    }

    sys_close(rfd);
    sys_close(wfd);
}

/* --- Test 6: exec with argv --- */
static void test_exec_argv(void) {
    const char *name = "exec with argv";

    const char *argv[] = { "/hello.elf", "--dummy", NULL };
    long child_pid = sys_exec("/hello.elf", argv);
    if (child_pid < 0) { fail(name, "exec failed"); return; }

    long status = sys_waitpid(child_pid);
    if (status == 0)
        pass(name);
    else
        fail(name, "child exit status non-zero");
}

int main(int argc, char **argv) {
    printf("\ns28test: running 6 tests\n");

    test_argv(argc, argv);
    test_fcntl_cloexec();
    test_fcntl_nonblock();
    test_nonblock_read();
    test_nonblock_write();
    test_exec_argv();

    if (tests_passed == tests_total)
        printf("s28test: ALL PASSED (%d/%d)\n", tests_passed, tests_total);
    else
        printf("s28test: %d/%d passed\n", tests_passed, tests_total);

    return (tests_passed == tests_total) ? 0 : 1;
}
