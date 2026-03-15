/*
 * s107test.c — Stage 107: /proc Filesystem + Symlink Tests
 *
 * Tests: VFS symlink operations, /proc virtual filesystem.
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

/* Helper to check if string contains substring */
static int strcontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* ===== Symlink Tests ===== */

static void test_symlink_basic(void) {
    printf("[1] Symlink: create and readlink\n");

    /* Create a test file */
    long fd = sys_open("/tmp_symtest", O_CREAT | O_RDWR);
    if (fd >= 0) {
        sys_fwrite(fd, "hello", 5);
        sys_close(fd);
    }

    /* Create symlink */
    long ret = sys_symlink("/tmp_symtest", "/tmp_symlink");
    check("symlink creation", ret == 0);

    /* Read the symlink target */
    char buf[256];
    long len = sys_readlink("/tmp_symlink", buf, 256);
    check("readlink returns target length", len > 0);
    if (len > 0) {
        buf[len] = '\0';
        check("readlink target correct", strcmp(buf, "/tmp_symtest") == 0);
    } else {
        check("readlink target correct", 0);
    }
}

static void test_symlink_follow(void) {
    printf("[2] Symlink: open file via symlink\n");

    /* Open the file through the symlink */
    long fd = sys_open("/tmp_symlink", O_RDONLY);
    check("open via symlink succeeds", fd >= 0);

    if (fd >= 0) {
        char buf[32];
        long n = sys_read(fd, buf, 31);
        if (n > 0) buf[n] = '\0';
        check("read through symlink correct", n == 5 && strcmp(buf, "hello") == 0);
        sys_close(fd);
    } else {
        check("read through symlink correct", 0);
    }
}

static void test_symlink_chain(void) {
    printf("[3] Symlink: chain (A -> B -> file)\n");

    /* /tmp_symlink already points to /tmp_symtest */
    /* Create /tmp_symchain -> /tmp_symlink */
    long ret = sys_symlink("/tmp_symlink", "/tmp_symchain");
    check("symlink chain creation", ret == 0);

    /* Open through chain */
    long fd = sys_open("/tmp_symchain", O_RDONLY);
    check("open via symlink chain", fd >= 0);

    if (fd >= 0) {
        char buf[32];
        long n = sys_read(fd, buf, 31);
        if (n > 0) buf[n] = '\0';
        check("read through chain correct", n == 5 && strcmp(buf, "hello") == 0);
        sys_close(fd);
    } else {
        check("read through chain correct", 0);
    }
}

static void test_symlink_eloop(void) {
    printf("[4] Symlink: circular detection (ELOOP)\n");

    /* Create circular symlinks: /loop_a -> /loop_b, /loop_b -> /loop_a */
    sys_symlink("/loop_b", "/loop_a");
    sys_symlink("/loop_a", "/loop_b");

    /* Try to open — should fail with ELOOP */
    long fd = sys_open("/loop_a", O_RDONLY);
    check("circular symlink returns error", fd < 0);
}

static void test_readlink_non_symlink(void) {
    printf("[5] Symlink: readlink on regular file fails\n");

    char buf[256];
    long ret = sys_readlink("/tmp_symtest", buf, 256);
    check("readlink on regular file fails", ret < 0);
}

/* ===== /proc Tests ===== */

static void test_proc_exists(void) {
    printf("[6] /proc: directory exists\n");

    long fd = sys_open("/proc", O_RDONLY);
    check("/proc directory exists", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_proc_self_status(void) {
    printf("[7] /proc/<pid>/status readable\n");

    long mypid = sys_getpid();
    char path[64];
    /* Build /proc/<pid>/status */
    int pos = 0;
    const char *prefix = "/proc/";
    while (*prefix) path[pos++] = *prefix++;
    /* itoa for pid */
    char pidstr[24];
    int pp = 0;
    long tmp = mypid;
    if (tmp == 0) pidstr[pp++] = '0';
    else {
        char rev[24]; int rp = 0;
        while (tmp > 0) { rev[rp++] = '0' + (tmp % 10); tmp /= 10; }
        while (rp > 0) pidstr[pp++] = rev[--rp];
    }
    pidstr[pp] = '\0';
    for (int i = 0; pidstr[i]; i++) path[pos++] = pidstr[i];
    const char *suffix = "/status";
    while (*suffix) path[pos++] = *suffix++;
    path[pos] = '\0';

    long fd = sys_open(path, O_RDONLY);
    check("/proc/<pid>/status opens", fd >= 0);

    if (fd >= 0) {
        char buf[1024];
        long n = sys_read(fd, buf, 1023);
        check("/proc/<pid>/status has content", n > 0);
        if (n > 0) {
            buf[n] = '\0';
            /* Should contain our PID */
            char needle[32];
            int np = 0;
            const char *p1 = "Pid:\t";
            while (*p1) needle[np++] = *p1++;
            for (int i = 0; pidstr[i]; i++) needle[np++] = pidstr[i];
            needle[np] = '\0';
            check("status contains Pid: <self>", strcontains(buf, needle));
        } else {
            check("status contains Pid: <self>", 0);
        }
        sys_close(fd);
    } else {
        check("/proc/<pid>/status has content", 0);
        check("status contains Pid: <self>", 0);
    }
}

static void test_proc_cmdline(void) {
    printf("[8] /proc/<pid>/cmdline readable\n");

    long mypid = sys_getpid();
    char path[64];
    int pos = 0;
    const char *prefix = "/proc/";
    while (*prefix) path[pos++] = *prefix++;
    long tmp = mypid;
    if (tmp == 0) path[pos++] = '0';
    else {
        char rev[24]; int rp = 0;
        while (tmp > 0) { rev[rp++] = '0' + (tmp % 10); tmp /= 10; }
        while (rp > 0) path[pos++] = rev[--rp];
    }
    const char *suffix = "/cmdline";
    while (*suffix) path[pos++] = *suffix++;
    path[pos] = '\0';

    long fd = sys_open(path, O_RDONLY);
    check("/proc/<pid>/cmdline opens", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_proc_fork_lifecycle(void) {
    printf("[9] /proc: child appears and disappears on fork/exit\n");

    long child = sys_fork();
    if (child == 0) {
        /* Child: just exit */
        sys_exit(0);
    }

    /* Parent: check /proc/<child>/ exists */
    char path[64];
    int pos = 0;
    const char *prefix = "/proc/";
    while (*prefix) path[pos++] = *prefix++;
    long tmp = child;
    if (tmp == 0) path[pos++] = '0';
    else {
        char rev[24]; int rp = 0;
        while (tmp > 0) { rev[rp++] = '0' + (tmp % 10); tmp /= 10; }
        while (rp > 0) path[pos++] = rev[--rp];
    }
    path[pos] = '\0';

    /* /proc/<child> should exist before waitpid */
    long fd = sys_open(path, O_RDONLY);
    check("/proc/<child> exists before reap", fd >= 0);
    if (fd >= 0) sys_close(fd);

    /* Reap child */
    sys_waitpid(child);

    /* /proc/<child> should be gone now */
    fd = sys_open(path, O_RDONLY);
    check("/proc/<child> gone after reap", fd < 0);
}

static void test_proc_readdir(void) {
    printf("[10] /proc: readdir lists entries\n");

    /* Read first entry from /proc */
    char name[256];
    unsigned char type;
    unsigned long size;
    /* Use raw readdir struct */
    char dirent[272];
    long ret = sys_readdir("/proc", 0, dirent);
    check("/proc has at least one entry", ret == 0);
}

int main(void) {
    printf("=== Stage 107: /proc + Symlink Test ===\n\n");

    /* Symlink tests */
    test_symlink_basic();
    test_symlink_follow();
    test_symlink_chain();
    test_symlink_eloop();
    test_readlink_non_symlink();

    /* /proc tests */
    test_proc_exists();
    test_proc_self_status();
    test_proc_cmdline();
    test_proc_fork_lifecycle();
    test_proc_readdir();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");

    return tests_failed;
}
