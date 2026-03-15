/*
 * s108test.c — Stage 108: Init System Tests
 *
 * Tests: /etc/inittab exists, init is pid 1 ancestor,
 *        environment inherited from init, /proc shows init.
 * Portable — no arch-specific code.
 */

#include "../../libc/libc.h"

static int tests_passed = 0;
static int tests_failed = 0;

static void check(const char *name, int cond) {
    if (cond) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        tests_failed++;
    }
}

static int strcontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void test_inittab_exists(void) {
    printf("[1] /etc/inittab exists and readable\n");
    long fd = sys_open("/etc/inittab", 0);
    check("/etc/inittab opens", fd >= 0);

    if (fd >= 0) {
        char buf[512];
        long n = sys_read(fd, buf, 511);
        check("/etc/inittab has content", n > 0);
        if (n > 0) {
            buf[n] = '\0';
            check("inittab contains shell entry", strcontains(buf, "shell"));
            check("inittab contains serviced entry", strcontains(buf, "serviced"));
        } else {
            check("inittab contains shell entry", 0);
            check("inittab contains serviced entry", 0);
        }
        sys_close(fd);
    } else {
        check("/etc/inittab has content", 0);
        check("inittab contains shell entry", 0);
        check("inittab contains serviced entry", 0);
    }
}

static void test_init_is_ancestor(void) {
    printf("[2] Init is pid 1 (our ancestor)\n");

    /* We're launched from shell, which was launched from init.
     * Check /proc/1/status exists and contains init */
    long fd = sys_open("/proc/1/status", 0);
    check("/proc/1/status exists (init is pid 1)", fd >= 0);

    if (fd >= 0) {
        char buf[1024];
        long n = sys_read(fd, buf, 1023);
        if (n > 0) {
            buf[n] = '\0';
            check("pid 1 is running", strcontains(buf, "Pid:\t1"));
        } else {
            check("pid 1 is running", 0);
        }
        sys_close(fd);
    } else {
        check("pid 1 is running", 0);
    }
}

static void test_env_inherited(void) {
    printf("[3] Environment inherited from init\n");

    char val[64];
    val[0] = '\0';
    long ret = sys_getenv("LIMNX_VERSION", val, 64);
    check("LIMNX_VERSION env set", ret >= 0);
    check("version is 0.108", strcmp(val, "0.108") == 0);
}

static void test_fork_from_init_child(void) {
    printf("[4] Fork works from init's grandchild\n");

    long child = sys_fork();
    if (child == 0) {
        sys_exit(42);
    }
    long status = sys_waitpid(child);
    check("fork+waitpid from init grandchild", status == 42);
}

static void test_proc_shows_services(void) {
    printf("[5] /proc shows running services\n");

    /* readdir /proc should have multiple entries (init, shell, possibly serviced) */
    char dirent[272];
    long ret1 = sys_readdir("/proc", 0, dirent);
    long ret2 = sys_readdir("/proc", 1, dirent);
    check("/proc has at least 2 entries", ret1 == 0 && ret2 == 0);
}

int main(void) {
    printf("=== Stage 108: Init System Test ===\n\n");

    test_inittab_exists();
    test_init_is_ancestor();
    test_env_inherited();
    test_fork_from_init_child();
    test_proc_shows_services();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");

    return tests_failed;
}
