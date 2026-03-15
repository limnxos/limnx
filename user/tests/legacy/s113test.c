/*
 * s113test.c — Stage 113: FIFOs + User Login Tests
 *
 * Tests: mkfifo, FIFO read/write, /etc/passwd, whoami, uid/gid.
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

static int strcontains(const char *h, const char *n) {
    if (!h || !n) return 0;
    for (int i = 0; h[i]; i++) {
        int j = 0;
        while (n[j] && h[i+j] == n[j]) j++;
        if (!n[j]) return 1;
    }
    return 0;
}

static void test_mkfifo(void) {
    printf("[1] mkfifo: create named pipe\n");
    long ret = sys_mkfifo("/testfifo113");
    check("mkfifo succeeds", ret == 0);
}

static void test_fifo_readwrite(void) {
    printf("[2] FIFO: read/write via open\n");

    /* Open FIFO for writing first (creates pipe) */
    long wfd = sys_open("/testfifo113", 1);  /* O_WRONLY */
    check("FIFO opens for writing", wfd >= 0);

    if (wfd >= 0) {
        /* Write some data */
        sys_fwrite(wfd, "fifo!", 5);

        /* Open FIFO for reading (same pipe) */
        long rfd = sys_open("/testfifo113", 0);  /* O_RDONLY */
        check("FIFO opens for reading", rfd >= 0);

        if (rfd >= 0) {
            char buf[32];
            long n = sys_read(rfd, buf, 31);
            if (n > 0) buf[n] = '\0';
            check("FIFO read returns data", n == 5);
            check("FIFO data correct", n == 5 && strcmp(buf, "fifo!") == 0);
            sys_close(rfd);
        } else {
            check("FIFO read returns data", 0);
            check("FIFO data correct", 0);
        }
        sys_close(wfd);
    } else {
        check("FIFO opens for reading", 0);
        check("FIFO read returns data", 0);
        check("FIFO data correct", 0);
    }
}

static void test_passwd_exists(void) {
    printf("[3] /etc/passwd exists\n");

    long fd = sys_open("/etc/passwd", 0);
    check("/etc/passwd opens", fd >= 0);

    if (fd >= 0) {
        char buf[512];
        long n = sys_read(fd, buf, 511);
        check("/etc/passwd has content", n > 0);
        if (n > 0) {
            buf[n] = '\0';
            check("passwd contains root entry", strcontains(buf, "root:x:0:0"));
        } else {
            check("passwd contains root entry", 0);
        }
        sys_close(fd);
    } else {
        check("/etc/passwd has content", 0);
        check("passwd contains root entry", 0);
    }
}

static void test_uid_gid(void) {
    printf("[4] UID/GID operations\n");

    long uid = sys_getuid();
    long gid = sys_getgid();
    check("getuid returns value", uid >= 0);
    check("getgid returns value", gid >= 0);
    /* We're running as root (uid 0) since init doesn't login */
    check("running as root (uid 0)", uid == 0);
}

static void test_whoami(void) {
    printf("[5] whoami program\n");

    long pid = sys_fork();
    if (pid == 0) {
        const char *argv[] = {"whoami.elf", (void *)0};
        sys_execve("/whoami.elf", argv);
        sys_exit(127);
    }
    long st = sys_waitpid(pid);
    check("whoami.elf executes", st == 0);
}

static void test_login_exists(void) {
    printf("[6] login program in initrd\n");

    long fd = sys_open("/login.elf", 0);
    check("login.elf exists", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_home_dir(void) {
    printf("[7] Home directory\n");

    long fd = sys_open("/root", 0);
    check("/root directory exists", fd >= 0);
    if (fd >= 0) sys_close(fd);
}

static void test_mkfifo_duplicate(void) {
    printf("[8] mkfifo: duplicate fails\n");
    long ret = sys_mkfifo("/testfifo113");
    check("duplicate mkfifo fails", ret < 0);
}

int main(void) {
    printf("=== Stage 113: FIFOs + User Login Test ===\n\n");

    test_mkfifo();
    test_fifo_readwrite();
    test_passwd_exists();
    test_uid_gid();
    test_whoami();
    test_login_exists();
    test_home_dir();
    test_mkfifo_duplicate();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_failed);
    if (tests_failed == 0)
        printf("ALL TESTS PASSED\n");

    return tests_failed;
}
